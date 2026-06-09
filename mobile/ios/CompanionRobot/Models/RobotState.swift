import Foundation

/// Represents a discovered BLE robot device.
struct DiscoveredDevice: Identifiable, Hashable {
    let id: UUID
    let name: String
    let peripheralId: UUID
    let rssi: Int
    var isConnected: Bool = false

    init(name: String, peripheralId: UUID, rssi: Int, isConnected: Bool = false) {
        self.id = peripheralId
        self.name = name
        self.peripheralId = peripheralId
        self.rssi = rssi
        self.isConnected = isConnected
    }
}

/// Real-time robot state.
struct RobotState {
    var deviceId: String = ""
    var name: String = ""
    var isConnected: Bool = false
    var battery: Int = 0
    var rssi: Int = 0
    var capabilities: [String: Bool] = [:]
    var firmwareVersion: String = ""
    var lastHeartbeat: Date = Date()

    // Sensor data
    var temperature: Float = 0
    var humidity: Float = 0
    var obstacleDistanceCm: Int = 0
    var imuPitch: Float = 0
    var imuRoll: Float = 0
}

/// Dialogue entry for the chat interface.
struct DialogueEntry: Identifiable {
    let id = UUID()
    let role: String // "user" or "robot"
    let text: String
    let timestamp: Date = Date()
}

/// App overall UI state.
struct AppUiState {
    var discoveredDevices: [DiscoveredDevice] = []
    var isScanning: Bool = false
    var connectedRobot: RobotState = RobotState()
    var wsConnected: Bool = false
    var wsUrl: String = "ws://gateway.local:8080"
    var dialogueHistory: [DialogueEntry] = []
    var isOfflineMode: Bool = false
    var errorMessage: String?
}
