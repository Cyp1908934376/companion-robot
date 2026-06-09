package com.companion.robot.viewmodel

import android.app.Application
import android.bluetooth.BluetoothAdapter
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.companion.robot.ble.BcpCodec
import com.companion.robot.ble.BleEvent
import com.companion.robot.ble.BleManager
import com.companion.robot.ble.DiscoveredDevice
import com.companion.robot.ws.WsClient
import com.companion.robot.ws.WsEvent
import kotlinx.coroutines.flow.*
import kotlinx.coroutines.launch

/**
 * Shared ViewModel for the entire app.
 *
 * State:
 *   - BLE scanning/discovery
 *   - Connected robot info + real-time data
 *   - WebSocket connection state
 *   - Dialogue history
 */

data class RobotState(
    val deviceId: String = "",
    val name: String = "",
    val isConnected: Boolean = false,
    val battery: Int = 0,
    val rssi: Int = 0,
    val capabilities: Map<String, Boolean> = emptyMap(),
    val firmwareVersion: String = "",
    val lastHeartbeat: Long = 0L,
    // Sensor data
    val temperature: Float = 0f,
    val humidity: Float = 0f,
    val obstacleDistanceCm: Int = 0,
    val imuPitch: Float = 0f,
    val imuRoll: Float = 0f,
)

data class DialogueEntry(
    val role: String, // "user" or "robot"
    val text: String,
    val timestamp: Long = System.currentTimeMillis()
)

data class AppUiState(
    val discoveredDevices: List<DiscoveredDevice> = emptyList(),
    val isScanning: Boolean = false,
    val connectedRobot: RobotState = RobotState(),
    val wsConnected: Boolean = false,
    val wsUrl: String = "ws://gateway.local:8080",
    val dialogueHistory: List<DialogueEntry> = emptyList(),
    val isOfflineMode: Boolean = false,
    val errorMessage: String? = null,
)

class RobotViewModel(application: Application) : AndroidViewModel(application) {

    private val _uiState = MutableStateFlow(AppUiState())
    val uiState: StateFlow<AppUiState> = _uiState.asStateFlow()

    private val bleManager = BleManager(application)
    private val wsClient = WsClient()
    private val bluetoothAdapter: BluetoothAdapter? = BluetoothAdapter.getDefaultAdapter()

    init {
        // Observe BLE events
        viewModelScope.launch {
            bleManager.events.collect { event ->
                handleBleEvent(event)
            }
        }

        // Observe WebSocket events
        viewModelScope.launch {
            wsClient.events.collect { event ->
                handleWsEvent(event)
            }
        }
    }

    // ── BLE actions ────────────────────────────────────────────

    fun startScan() {
        val adapter = bluetoothAdapter
        if (adapter?.isEnabled == true) {
            bleManager.startScan(adapter)
        } else {
            _uiState.update { it.copy(errorMessage = "Bluetooth is not enabled") }
        }
    }

    fun stopScan() {
        val adapter = bluetoothAdapter
        if (adapter != null) {
            bleManager.stopScan(adapter)
            _uiState.update { it.copy(isScanning = false) }
        }
    }

    fun connectToDevice(device: DiscoveredDevice) {
        bleManager.connect(device.device)
    }

    fun disconnectDevice() {
        bleManager.disconnect()
        _uiState.update { it.copy(connectedRobot = RobotState()) }
    }

    // ── WebSocket actions ──────────────────────────────────────

    fun connectWs(url: String, token: String) {
        _uiState.update { it.copy(wsUrl = url) }
        wsClient.connect(url, token)
    }

    fun disconnectWs() {
        wsClient.disconnect()
        _uiState.update { it.copy(wsConnected = false) }
    }

    // ── Robot control commands ─────────────────────────────────

    fun sendMoveCommand(direction: String, speed: Int, durationMs: Int = 0) {
        val frame = BcpCodec.BcpFrame.create(
            seqNo = 0,
            command = BcpCodec.BcpCommand(
                cmdId = when (direction) {
                    "forward" -> 0x0101
                    "backward" -> 0x0102
                    "left" -> 0x0103
                    "right" -> 0x0104
                    else -> 0x0100
                },
                payload = byteArrayOf(
                    direction.first().code.toByte(),
                    speed.toByte(),
                    (durationMs and 0xFF).toByte(),
                    ((durationMs shr 8) and 0xFF).toByte()
                )
            )
        )
        sendFrame(frame)
    }

    fun sendStopCommand() {
        val frame = BcpCodec.BcpFrame.create(
            seqNo = 0,
            command = BcpCodec.BcpCommand(cmdId = 0x0105, payload = byteArrayOf(0))
        )
        sendFrame(frame)
    }

    fun sendLedCommand(mode: Int, r: Int, g: Int, b: Int, speed: Int = 100) {
        val frame = BcpCodec.BcpFrame.create(
            seqNo = 0,
            command = BcpCodec.BcpCommand(
                cmdId = 0x0201,
                payload = byteArrayOf(mode.toByte(), speed.toByte(), r.toByte(), g.toByte(), b.toByte())
            )
        )
        sendFrame(frame)
    }

    fun sendLedOff() {
        val frame = BcpCodec.BcpFrame.create(
            seqNo = 0,
            command = BcpCodec.BcpCommand(cmdId = 0x0203, payload = byteArrayOf())
        )
        sendFrame(frame)
    }

    fun sendFaceExpression(expression: Int) {
        val frame = BcpCodec.BcpFrame.create(
            seqNo = 0,
            command = BcpCodec.BcpCommand(
                cmdId = 0x0204,
                payload = byteArrayOf(expression.toByte())
            )
        )
        sendFrame(frame)
    }

    fun sendSpeakCommand(text: String) {
        val textBytes = text.toByteArray(Charsets.UTF_8).take(255).toByteArray()
        val payload = byteArrayOf(textBytes.size.toByte()) + textBytes
        val frame = BcpCodec.BcpFrame.create(
            seqNo = 0,
            command = BcpCodec.BcpCommand(cmdId = 0x0206, payload = payload)
        )
        sendFrame(frame)

        // Add to dialogue history
        addDialogueEntry(DialogueEntry(role = "user", text = text))
    }

    fun sendServoCommand(panAngle: Int, tiltAngle: Int) {
        val frame = BcpCodec.BcpFrame.create(
            seqNo = 0,
            command = BcpCodec.BcpCommand(
                cmdId = 0x0107,
                payload = byteArrayOf(
                    (panAngle and 0xFF).toByte(),
                    ((panAngle shr 8) and 0xFF).toByte(),
                    (tiltAngle and 0xFF).toByte(),
                    ((tiltAngle shr 8) and 0xFF).toByte()
                )
            )
        )
        sendFrame(frame)
    }

    fun clearError() {
        _uiState.update { it.copy(errorMessage = null) }
    }

    fun addDialogueEntry(entry: DialogueEntry) {
        _uiState.update {
            it.copy(dialogueHistory = it.dialogueHistory + entry)
        }
    }

    // ── Private helpers ────────────────────────────────────────

    private fun sendFrame(frame: BcpCodec.BcpFrame) {
        // Try WebSocket first (bridge mode), then BLE
        if (_uiState.value.wsConnected) {
            val buf = ByteArray(BcpCodec.MAX_FRAME_LEN)
            val len = BcpCodec.encode(frame, buf)
            wsClient.sendBcpFrame(buf.copyOf(len))
        } else if (_uiState.value.connectedRobot.isConnected) {
            bleManager.sendBcpFrame(frame)
        }
    }

    private fun handleBleEvent(event: BleEvent) {
        when (event) {
            is BleEvent.DeviceDiscovered -> {
                _uiState.update {
                    it.copy(
                        discoveredDevices = it.discoveredDevices + event.device
                    )
                }
            }
            is BleEvent.ConnectionStateChanged -> {
                if (event.state == android.bluetooth.BluetoothProfile.STATE_CONNECTED) {
                    _uiState.update {
                        it.copy(
                            connectedRobot = it.connectedRobot.copy(
                                deviceId = event.deviceId,
                                isConnected = true
                            )
                        )
                    }
                } else {
                    _uiState.update {
                        it.copy(
                            connectedRobot = RobotState(),
                            isOfflineMode = true
                        )
                    }
                }
            }
            is BleEvent.FrameReceived -> {
                // Parse BCP frame, update robot state
                handleBcpFrame(event.data)
            }
            is BleEvent.Error -> {
                _uiState.update { it.copy(errorMessage = event.message) }
            }
            else -> {}
        }
    }

    private fun handleWsEvent(event: WsEvent) {
        when (event) {
            is WsEvent.Connected -> {
                _uiState.update { it.copy(wsConnected = true, isOfflineMode = false) }
            }
            is WsEvent.Disconnected -> {
                _uiState.update { it.copy(wsConnected = false) }
            }
            is WsEvent.FrameReceived -> {
                handleBcpFrame(event.data)
            }
            is WsEvent.Error -> {
                _uiState.update { it.copy(errorMessage = event.message) }
            }
            else -> {}
        }
    }

    private fun handleBcpFrame(data: ByteArray) {
        if (data.size < BcpCodec.HEADER_LEN) return

        // Verify magic
        if (data[0] != BcpCodec.MAGIC) return

        // In production: decode full BCP frame, update robot state from commands
        // For now: just log
    }
}
