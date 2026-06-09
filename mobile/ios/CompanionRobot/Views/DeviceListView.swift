import SwiftUI

struct DeviceListView: View {
    @EnvironmentObject var viewModel: RobotViewModel

    var body: some View {
        VStack(spacing: 16) {
            // Connection status card
            ConnectionStatusCard(uiState: viewModel.uiState)

            // Scan button
            HStack {
                Button(action: {
                    if viewModel.uiState.isScanning {
                        viewModel.stopScan()
                    } else {
                        viewModel.startScan()
                    }
                }) {
                    HStack {
                        Image(systemName: viewModel.uiState.isScanning ? "stop.circle" : "antenna.radiowaves.left.and.right")
                        Text(viewModel.uiState.isScanning ? "Scanning..." : "Scan BLE")
                    }
                    .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)

                Button(action: {
                    if viewModel.uiState.wsConnected {
                        viewModel.disconnectWs()
                    } else {
                        viewModel.connectWs(url: "ws://gateway.local:8080", token: "token")
                    }
                }) {
                    HStack {
                        Image(systemName: viewModel.uiState.wsConnected ? "wifi.slash" : "wifi")
                        Text(viewModel.uiState.wsConnected ? "Disconnect" : "Connect WS")
                    }
                    .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
            }
            .padding(.horizontal)

            // Device list
            VStack(alignment: .leading) {
                Text("Discovered Devices")
                    .font(.headline)
                    .padding(.horizontal)

                if viewModel.uiState.discoveredDevices.isEmpty {
                    VStack(spacing: 16) {
                        Image(systemName: "robot")
                            .font(.system(size: 48))
                            .foregroundColor(.secondary)
                        Text(viewModel.uiState.isScanning
                             ? "Scanning for robots..."
                             : "No devices found.\nTap 'Scan BLE' to start.")
                            .multilineTextAlignment(.center)
                            .foregroundColor(.secondary)
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.top, 40)
                } else {
                    List {
                        ForEach(viewModel.uiState.discoveredDevices) { device in
                            DeviceRow(device: device)
                                .onTapGesture {
                                    viewModel.connectToDevice(device)
                                }
                        }
                    }
                }
            }

            // Error message
            if let error = viewModel.uiState.errorMessage {
                HStack {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundColor(.red)
                    Text(error)
                        .font(.caption)
                        .foregroundColor(.red)
                }
                .padding()
                .background(.red.opacity(0.1))
                .cornerRadius(8)
                .padding(.horizontal)
                .onTapGesture { viewModel.clearError() }
            }

            Spacer()
        }
        .navigationTitle("Companion Robot")
    }
}

struct ConnectionStatusCard: View {
    let uiState: AppUiState

    var body: some View {
        HStack(spacing: 16) {
            StatusItem(
                label: "BLE",
                value: uiState.connectedRobot.isConnected ? "Connected" : "Disconnected",
                color: uiState.connectedRobot.isConnected ? .green : .gray
            )
            StatusItem(
                label: "WS",
                value: uiState.wsConnected ? "Connected" : "Disconnected",
                color: uiState.wsConnected ? .green : .gray
            )
            StatusItem(
                label: "Mode",
                value: uiState.isOfflineMode ? "Offline" : "Online",
                color: uiState.isOfflineMode ? .orange : .blue
            )
        }
        .padding()
        .background(Color(.systemGray6))
        .cornerRadius(12)
        .padding(.horizontal)
    }
}

struct StatusItem: View {
    let label: String
    let value: String
    let color: Color

    var body: some View {
        VStack(spacing: 4) {
            Circle()
                .fill(color)
                .frame(width: 10, height: 10)
            Text(value)
                .font(.caption)
                .fontWeight(.medium)
            Text(label)
                .font(.caption2)
                .foregroundColor(.secondary)
        }
        .frame(maxWidth: .infinity)
    }
}

struct DeviceRow: View {
    let device: DiscoveredDevice

    var body: some View {
        HStack {
            Image(systemName: "robot")
                .font(.title2)
                .foregroundColor(device.isConnected ? .green : .secondary)

            VStack(alignment: .leading) {
                Text(device.name)
                    .font(.body)
                    .fontWeight(.medium)
                Text(device.peripheralId.uuidString)
                    .font(.caption)
                    .foregroundColor(.secondary)
            }

            Spacer()

            Text("\(device.rssi) dBm")
                .font(.caption)
                .foregroundColor(device.rssi > -60 ? .green : .secondary)
        }
        .padding(.vertical, 4)
    }
}
