import SwiftUI

struct RobotControlView: View {
    @EnvironmentObject var viewModel: RobotViewModel
    let deviceId: String
    let onDialogue: () -> Void

    var body: some View {
        ScrollView {
            VStack(spacing: 24) {
                // Status bar
                HStack(spacing: 16) {
                    StatusChip(label: "Battery", value: "\(viewModel.uiState.connectedRobot.battery)%",
                               icon: "battery.75")
                    StatusChip(label: "RSSI", value: "\(viewModel.uiState.connectedRobot.rssi) dBm",
                               icon: "antenna.radiowaves.left.and.right")
                    StatusChip(label: "FW", value: viewModel.uiState.connectedRobot.firmwareVersion.isEmpty
                               ? "v0.1.0" : viewModel.uiState.connectedRobot.firmwareVersion,
                               icon: "cpu")
                }
                .padding()
                .background(Color(.systemGray6))
                .cornerRadius(12)
                .padding(.horizontal)

                // Directional control
                VStack(spacing: 8) {
                    Text("Movement").font(.headline)

                    DirectionPad(
                        onForward: { viewModel.sendMoveCommand(direction: "forward", speed: 128, durationMs: 500) },
                        onBackward: { viewModel.sendMoveCommand(direction: "backward", speed: 100, durationMs: 500) },
                        onLeft: { viewModel.sendMoveCommand(direction: "left", speed: 100, durationMs: 300) },
                        onRight: { viewModel.sendMoveCommand(direction: "right", speed: 100, durationMs: 300) },
                        onStop: { viewModel.sendStopCommand() }
                    )
                }

                // Expression controls
                VStack(spacing: 12) {
                    Text("Expression").font(.headline)

                    // LED colors
                    HStack(spacing: 8) {
                        ColorButton("Red", color: .red) {
                            viewModel.sendLedCommand(mode: 0, r: 255, g: 0, b: 0)
                        }
                        ColorButton("Green", color: .green) {
                            viewModel.sendLedCommand(mode: 0, r: 0, g: 255, b: 0)
                        }
                        ColorButton("Blue", color: .blue) {
                            viewModel.sendLedCommand(mode: 0, r: 0, g: 0, b: 255)
                        }
                        ColorButton("Off", color: .gray) {
                            viewModel.sendLedOff()
                        }
                    }

                    // Face expressions
                    HStack(spacing: 8) {
                        ExpressionButton("😊 Happy") { viewModel.sendFaceExpression(0) }
                        ExpressionButton("😢 Sad") { viewModel.sendFaceExpression(1) }
                        ExpressionButton("😠 Angry") { viewModel.sendFaceExpression(2) }
                    }
                    HStack(spacing: 8) {
                        ExpressionButton("😮 Surprised") { viewModel.sendFaceExpression(3) }
                        ExpressionButton("😐 Neutral") { viewModel.sendFaceExpression(4) }
                    }
                }
                .padding(.horizontal)

                // Sensor data
                VStack(alignment: .leading, spacing: 8) {
                    Text("Sensors").font(.headline)
                    HStack {
                        SensorRow(label: "Temp", value: "\(String(format: "%.1f", viewModel.uiState.connectedRobot.temperature))°C")
                        SensorRow(label: "Humidity", value: "\(String(format: "%.1f", viewModel.uiState.connectedRobot.humidity))%")
                        SensorRow(label: "Obstacle", value: "\(viewModel.uiState.connectedRobot.obstacleDistanceCm)cm")
                    }
                    HStack {
                        SensorRow(label: "Pitch", value: "\(String(format: "%.1f", viewModel.uiState.connectedRobot.imuPitch))°")
                        SensorRow(label: "Roll", value: "\(String(format: "%.1f", viewModel.uiState.connectedRobot.imuRoll))°")
                    }
                }
                .padding()
                .background(Color(.systemGray6))
                .cornerRadius(12)
                .padding(.horizontal)
            }
        }
        .navigationTitle(viewModel.uiState.connectedRobot.name.isEmpty
                         ? "Robot Control" : viewModel.uiState.connectedRobot.name)
        .toolbar {
            ToolbarItem(placement: .navigationBarTrailing) {
                Button("Chat") { onDialogue() }
            }
        }
    }
}

// MARK: - Subviews

struct StatusChip: View {
    let label: String
    let value: String
    let icon: String

    var body: some View {
        VStack(spacing: 4) {
            Image(systemName: icon)
                .font(.title3)
                .foregroundColor(.accentColor)
            Text(value)
                .font(.caption)
                .fontWeight(.bold)
            Text(label)
                .font(.caption2)
                .foregroundColor(.secondary)
        }
        .frame(maxWidth: .infinity)
    }
}

struct DirectionPad: View {
    let onForward: () -> Void
    let onBackward: () -> Void
    let onLeft: () -> Void
    let onRight: () -> Void
    let onStop: () -> Void

    var body: some View {
        VStack(spacing: 4) {
            DirectionButton("chevron.up", action: onForward)

            HStack(spacing: 16) {
                DirectionButton("chevron.left", action: onLeft)

                Button(action: onStop) {
                    Image(systemName: "stop.fill")
                        .font(.title2)
                        .foregroundColor(.white)
                        .frame(width: 56, height: 56)
                        .background(Circle().fill(.red))
                }

                DirectionButton("chevron.right", action: onRight)
            }

            DirectionButton("chevron.down", action: onBackward)
        }
    }
}

struct DirectionButton: View {
    let icon: String
    let action: () -> Void

    init(_ icon: String, action: @escaping () -> Void) {
        self.icon = icon
        self.action = action
    }

    var body: some View {
        Button(action: action) {
            Image(systemName: icon)
                .font(.title2)
                .frame(width: 56, height: 56)
                .background(Circle().fill(Color(.systemGray5)))
        }
    }
}

struct ColorButton: View {
    let label: String
    let color: Color
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Text(label)
                .font(.caption)
                .fontWeight(.medium)
                .foregroundColor(.white)
                .frame(maxWidth: .infinity)
                .padding(.vertical, 10)
                .background(color)
                .cornerRadius(8)
        }
    }
}

struct ExpressionButton: View {
    let label: String
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Text(label)
                .font(.caption)
                .frame(maxWidth: .infinity)
                .padding(.vertical, 10)
                .background(Color(.systemGray5))
                .cornerRadius(8)
        }
    }
}

struct SensorRow: View {
    let label: String
    let value: String

    var body: some View {
        VStack {
            Text(value).font(.caption).fontWeight(.medium)
            Text(label).font(.caption2).foregroundColor(.secondary)
        }
        .frame(maxWidth: .infinity)
    }
}
