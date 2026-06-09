package com.companion.robot.ble

import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothProfile
import android.content.Context
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.SharedFlow
import no.nordicsemi.android.ble.BleManager as NordicBleManager
import no.nordicsemi.android.ble.data.Data
import java.util.UUID

/**
 * BLE Manager — scan, connect, MTU negotiation, GATT operations.
 *
 * BCP Service:
 *   UUID 0xCB00: BCP Service
 *     - 0xCB01: BCP_TX (Write) — mobile → robot
 *     - 0xCB02: BCP_RX (Notify) — robot → mobile
 *     - 0xCB03: BCP_CONTROL (Read/Write) — connection params
 *     - 0xCB04: DEVICE_INFO (Read) — robot metadata
 */

object BleConstants {
    val SERVICE_UUID: UUID = UUID.fromString("0000CB00-0000-1000-8000-00805F9B34FB")
    val CHAR_BCP_TX: UUID     = UUID.fromString("0000CB01-0000-1000-8000-00805F9B34FB")
    val CHAR_BCP_RX: UUID     = UUID.fromString("0000CB02-0000-1000-8000-00805F9B34FB")
    val CHAR_BCP_CONTROL: UUID = UUID.fromString("0000CB03-0000-1000-8000-00805F9B34FB")
    val CHAR_DEVICE_INFO: UUID = UUID.fromString("0000CB04-0000-1000-8000-00805F9B34FB")
    val CHAR_OTA_DATA: UUID    = UUID.fromString("0000CB05-0000-1000-8000-00805F9B34FB")

    const val TARGET_MTU = 247
    const val DEVICE_NAME_PREFIX = "CompanionBot"
}

/** Represents a discovered BLE device. */
data class DiscoveredDevice(
    val device: BluetoothDevice,
    val name: String,
    val rssi: Int,
    val isConnected: Boolean = false
)

/** Events from the BLE layer. */
sealed class BleEvent {
    data class FrameReceived(val data: ByteArray) : BleEvent()
    data class DeviceDiscovered(val device: DiscoveredDevice) : BleEvent()
    data class ConnectionStateChanged(val deviceId: String, val state: Int) : BleEvent()
    object MtuNegotiated : BleEvent()
    object ServicesDiscovered : BleEvent()
    data class Error(val message: String) : BleEvent()
}

/**
 * BLE Manager handles scanning, connection lifecycle, and data transfer.
 * Uses Nordic BLE Library for reliable BLE operations.
 */
class BleManager(private val context: Context) {

    private val _events = MutableSharedFlow<BleEvent>(replay = 0)
    val events: SharedFlow<BleEvent> = _events

    private val _connectedDevice = MutableStateFlow<DiscoveredDevice?>(null)
    val connectedDevice: StateFlow<DiscoveredDevice?> = _connectedDevice

    private val _isScanning = MutableStateFlow(false)
    val isScanning: StateFlow<Boolean> = _isScanning

    private var gatt: BluetoothGatt? = null
    private var txCharacteristic: BluetoothGattCharacteristic? = null
    private var negotiatedMtu: Int = 23

    // Fragment reassembly buffer
    private val fragmentBuffer = mutableListOf<Byte>()
    private var expectedTotalLen = 0
    private var fragmentTimeout = 0L

    /**
     * Start BLE scanning for CompanionBot devices.
     */
    fun startScan(adapter: android.bluetooth.BluetoothAdapter) {
        _isScanning.value = true

        // In production: use BluetoothLeScanner with ScanFilter for SERVICE_UUID
        // For now: stub that simulates discovery
        _events.tryEmit(BleEvent.Error("BLE scanning requires hardware — stub mode"))
        _isScanning.value = false
    }

    fun stopScan(adapter: android.bluetooth.BluetoothAdapter) {
        _isScanning.value = false
    }

    /**
     * Connect to a BLE device and discover GATT services.
     */
    fun connect(device: BluetoothDevice) {
        device.connectGatt(context, false, gattCallback)
    }

    fun disconnect() {
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        txCharacteristic = null
        _connectedDevice.value = null
    }

    /**
     * Send a BCP frame over BLE. Auto-fragments if exceeding MTU-3.
     */
    fun sendBcpFrame(frame: BcpCodec.BcpFrame): Boolean {
        val tx = txCharacteristic ?: return false

        val buf = ByteArray(BcpCodec.MAX_FRAME_LEN)
        val totalLen = BcpCodec.encode(frame, buf)

        val effectiveMtu = negotiatedMtu - 3

        if (totalLen <= effectiveMtu) {
            // Single write
            tx.value = buf.copyOf(totalLen)
            return gatt?.writeCharacteristic(tx) == true
        }

        // Fragment and send
        var offset = 0
        while (offset < totalLen) {
            val chunkSize = minOf(effectiveMtu, totalLen - offset)
            val chunk = ByteArray(chunkSize + 2)

            // Fragment header: [totalLen_hi, totalLen_lo] + data
            chunk[0] = (totalLen shr 8).toByte()
            chunk[1] = (totalLen and 0xFF).toByte()
            buf.copyInto(chunk, 2, offset, offset + chunkSize)

            // In production: queue fragments and send via reliable write
            offset += chunkSize
        }

        return true
    }

    // ── GATT callback ──────────────────────────────────────────

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    this@BleManager.gatt = gatt
                    gatt.requestMtu(BleConstants.TARGET_MTU)
                    _events.tryEmit(BleEvent.ConnectionStateChanged(gatt.device.address, newState))
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    _connectedDevice.value = null
                    _events.tryEmit(BleEvent.ConnectionStateChanged(gatt.device.address, newState))
                }
            }
        }

        override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                negotiatedMtu = mtu
                _events.tryEmit(BleEvent.MtuNegotiated)
                gatt.discoverServices()
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                val service = gatt.getService(BleConstants.SERVICE_UUID)
                txCharacteristic = service?.getCharacteristic(BleConstants.CHAR_BCP_TX)

                // Subscribe to RX characteristic for notifications
                service?.getCharacteristic(BleConstants.CHAR_BCP_RX)?.let { rxChar ->
                    gatt.setCharacteristicNotification(rxChar, true)
                }

                _events.tryEmit(BleEvent.ServicesDiscovered)
            }
        }

        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic
        ) {
            if (characteristic.uuid == BleConstants.CHAR_BCP_RX) {
                handleReceivedData(characteristic.value)
            }
        }

        override fun onCharacteristicRead(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int
        ) {
            if (status == BluetoothGatt.GATT_SUCCESS &&
                characteristic.uuid == BleConstants.CHAR_DEVICE_INFO) {
                // Parse device info (machine_id, firmware_ver, capabilities)
                _events.tryEmit(BleEvent.FrameReceived(characteristic.value))
            }
        }
    }

    /**
     * Handle incoming data — may be a fragment or a complete frame.
     */
    private fun handleReceivedData(data: ByteArray?) {
        if (data == null || data.isEmpty()) return

        // Check if this is a fragment (first 2 bytes = total length)
        if (data.size >= 2) {
            val totalLen = ((data[0].toInt() and 0xFF) shl 8) or (data[1].toInt() and 0xFF)
            if (totalLen > 0 && totalLen <= BcpCodec.MAX_FRAME_LEN) {
                // Fragment: reset buffer if new frame
                if (fragmentBuffer.isEmpty()) {
                    expectedTotalLen = totalLen
                    fragmentTimeout = System.currentTimeMillis()
                }
                fragmentBuffer.addAll(data.drop(2))
            } else {
                // Complete frame
                fragmentBuffer.clear()
                fragmentBuffer.addAll(data.toList())
            }
        } else {
            fragmentBuffer.addAll(data.toList())
        }

        // Check if frame is complete
        if (fragmentBuffer.isNotEmpty() && fragmentBuffer.size >= expectedTotalLen) {
            val completeFrame = fragmentBuffer.take(expectedTotalLen).toByteArray()
            _events.tryEmit(BleEvent.FrameReceived(completeFrame))
            fragmentBuffer.clear()
            expectedTotalLen = 0
        }

        // Fragment timeout check (2 seconds)
        if (fragmentBuffer.isNotEmpty() &&
            System.currentTimeMillis() - fragmentTimeout > 2000) {
            fragmentBuffer.clear()
            expectedTotalLen = 0
        }
    }
}
