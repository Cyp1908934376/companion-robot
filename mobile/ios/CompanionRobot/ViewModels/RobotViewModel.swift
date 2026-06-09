import Foundation
import Combine
import SwiftUI

/// Shared ViewModel for the entire iOS app.
@MainActor
class RobotViewModel: ObservableObject {
    @Published var uiState = AppUiState()

    private let bleManager = BleManager()
    private let wsClient = WsClient()
    private var cancellables = Set<AnyCancellable>()

    func startUp() {
        // Observe BLE events
        bleManager.events
            .receive(on: DispatchQueue.main)
            .sink { [weak self] event in
                self?.handleBleEvent(event)
            }
            .store(in: &cancellables)

        // Observe WebSocket events
        wsClient.events
            .receive(on: DispatchQueue.main)
            .sink { [weak self] event in
                self?.handleWsEvent(event)
            }
            .store(in: &cancellables)
    }

    // MARK: - BLE actions

    func startScan() { bleManager.startScan() }
    func stopScan() { bleManager.stopScan() }

    func connectToDevice(_ device: DiscoveredDevice) {
        // Need to get the CBPeripheral reference
        // In production: store peripheral references in BleManager
    }

    func disconnectDevice() {
        bleManager.disconnect()
        uiState.connectedRobot = RobotState()
        uiState.isOfflineMode = true
    }

    // MARK: - WebSocket actions

    func connectWs(url: String, token: String) {
        uiState.wsUrl = url
        wsClient.connect(url: url, token: token)
    }

    func disconnectWs() {
        wsClient.disconnect()
        uiState.wsConnected = false
    }

    // MARK: - Robot commands

    func sendMoveCommand(direction: String, speed: Int, durationMs: Int = 0) {
        let cmdId: UInt16 = switch direction {
        case "forward":  0x0101
        case "backward": 0x0102
        case "left":     0x0103
        case "right":    0x0104
        default:         0x0100
        }

        var payload = Data()
        payload.append(direction.first?.asciiValue ?? 0)
        payload.append(UInt8(speed))
        payload.append(UInt8(durationMs & 0xFF))
        payload.append(UInt8((durationMs >> 8) & 0xFF))

        let cmd = BcpCodec.BcpCommand(cmdId: cmdId, payload: payload)
        let frame = BcpCodec.BcpFrame(seqNo: 0, commands: [cmd])
        sendFrame(frame)
    }

    func sendStopCommand() {
        let cmd = BcpCodec.BcpCommand(cmdId: BcpCodec.cmdStop, payload: Data([0]))
        let frame = BcpCodec.BcpFrame(seqNo: 0, commands: [cmd])
        sendFrame(frame)
    }

    func sendLedCommand(mode: UInt8, r: UInt8, g: UInt8, b: UInt8, speed: UInt8 = 100) {
        let payload = Data([mode, speed, r, g, b])
        let cmd = BcpCodec.BcpCommand(cmdId: BcpCodec.cmdLedSet, payload: payload)
        let frame = BcpCodec.BcpFrame(seqNo: 0, commands: [cmd])
        sendFrame(frame)
    }

    func sendLedOff() {
        let cmd = BcpCodec.BcpCommand(cmdId: BcpCodec.cmdLedOff, payload: Data())
        let frame = BcpCodec.BcpFrame(seqNo: 0, commands: [cmd])
        sendFrame(frame)
    }

    func sendFaceExpression(_ expression: UInt8) {
        let cmd = BcpCodec.BcpCommand(cmdId: BcpCodec.cmdFaceExpr, payload: Data([expression]))
        let frame = BcpCodec.BcpFrame(seqNo: 0, commands: [cmd])
        sendFrame(frame)
    }

    func sendServoCommand(panAngle: Int, tiltAngle: Int) {
        var payload = Data()
        payload.append(UInt8(panAngle & 0xFF))
        payload.append(UInt8((panAngle >> 8) & 0xFF))
        payload.append(UInt8(tiltAngle & 0xFF))
        payload.append(UInt8((tiltAngle >> 8) & 0xFF))

        let cmd = BcpCodec.BcpCommand(cmdId: BcpCodec.cmdServoSet, payload: payload)
        let frame = BcpCodec.BcpFrame(seqNo: 0, commands: [cmd])
        sendFrame(frame)
    }

    func sendSpeakCommand(_ text: String) {
        var payload = Data()
        let textData = text.data(using: .utf8)?.prefix(255) ?? Data()
        payload.append(UInt8(textData.count))
        payload.append(textData)

        let cmd = BcpCodec.BcpCommand(cmdId: BcpCodec.cmdSpeak, payload: payload)
        let frame = BcpCodec.BcpFrame(seqNo: 0, commands: [cmd])
        sendFrame(frame)

        // Add to dialogue
        uiState.dialogueHistory.append(DialogueEntry(role: "user", text: text))
    }

    func clearError() {
        uiState.errorMessage = nil
    }

    // MARK: - Private

    private func sendFrame(_ frame: BcpCodec.BcpFrame) {
        if uiState.wsConnected {
            let data = BcpCodec.encode(frame: frame)
            _ = wsClient.sendBcpFrame(data)
        } else if uiState.connectedRobot.isConnected {
            _ = bleManager.sendBcpFrame(frame)
        }
    }

    private func handleBleEvent(_ event: BleEvent) {
        switch event {
        case .deviceDiscovered(let device):
            if !uiState.discoveredDevices.contains(where: { $0.id == device.id }) {
                uiState.discoveredDevices.append(device)
            }
        case .connectionStateChanged(let deviceId, let state):
            if state == .connected {
                uiState.connectedRobot.deviceId = deviceId
                uiState.connectedRobot.isConnected = true
                uiState.isOfflineMode = false
            } else {
                uiState.connectedRobot = RobotState()
                uiState.isOfflineMode = true
            }
        case .frameReceived(let data):
            handleBcpFrame(data)
        case .error(let message):
            uiState.errorMessage = message
        default:
            break
        }
    }

    private func handleWsEvent(_ event: WsEvent) {
        switch event {
        case .connected:
            uiState.wsConnected = true
            uiState.isOfflineMode = false
        case .disconnected:
            uiState.wsConnected = false
        case .frameReceived(let data):
            handleBcpFrame(data)
        case .error(let message):
            uiState.errorMessage = message
        default:
            break
        }
    }

    private func handleBcpFrame(_ data: Data) {
        guard data.count >= BcpCodec.headerLen else { return }
        guard data[0] == BcpCodec.magic else { return }
        // In production: decode full BCP frame, update state from commands
    }
}
