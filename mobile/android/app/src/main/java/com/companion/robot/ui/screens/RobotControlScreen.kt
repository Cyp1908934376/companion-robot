package com.companion.robot.ui.screens

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.companion.robot.viewmodel.RobotViewModel

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun RobotControlScreen(
    viewModel: RobotViewModel,
    deviceId: String,
    onBack: () -> Unit,
    onDialogue: () -> Unit
) {
    val uiState by viewModel.uiState.collectAsState()

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(uiState.connectedRobot.name.ifEmpty { "Robot Control" }) },
                navigationIcon = {
                    TextButton(onClick = onBack) { Text("Back") }
                },
                actions = {
                    TextButton(onClick = onDialogue) { Text("Chat") }
                }
            )
        }
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(16.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            // Status bar
            Card(
                modifier = Modifier.fillMaxWidth(),
                colors = CardDefaults.cardColors(
                    containerColor = MaterialTheme.colorScheme.primaryContainer
                )
            ) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(16.dp),
                    horizontalArrangement = Arrangement.SpaceEvenly
                ) {
                    StatusChip("Battery", "${uiState.connectedRobot.battery}%")
                    StatusChip("RSSI", "${uiState.connectedRobot.rssi} dBm")
                    StatusChip("FW", uiState.connectedRobot.firmwareVersion.ifEmpty { "v0.1.0" })
                }
            }

            Spacer(modifier = Modifier.height(24.dp))

            // Directional control pad
            Text(
                text = "Movement",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Bold
            )

            Spacer(modifier = Modifier.height(12.dp))

            // Direction pad
            DirectionPad(
                onForward = { viewModel.sendMoveCommand("forward", 128, 500) },
                onBackward = { viewModel.sendMoveCommand("backward", 100, 500) },
                onLeft = { viewModel.sendMoveCommand("left", 100, 300) },
                onRight = { viewModel.sendMoveCommand("right", 100, 300) },
                onStop = { viewModel.sendStopCommand() }
            )

            Spacer(modifier = Modifier.height(24.dp))

            // Expression controls
            Text(
                text = "Expression",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Bold
            )

            Spacer(modifier = Modifier.height(8.dp))

            // LED colors
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                LedColorButton("Red", 0xFF0000) { viewModel.sendLedCommand(0, 255, 0, 0) }
                LedColorButton("Green", 0x00FF00) { viewModel.sendLedCommand(0, 0, 255, 0) }
                LedColorButton("Blue", 0x0000FF) { viewModel.sendLedCommand(0, 0, 0, 255) }
                LedColorButton("Off", 0x666666) { viewModel.sendLedOff() }
            }

            Spacer(modifier = Modifier.height(12.dp))

            // Face expressions
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                ExpressionButton("😊 Happy", 0) { viewModel.sendFaceExpression(0) }
                ExpressionButton("😢 Sad", 1) { viewModel.sendFaceExpression(1) }
                ExpressionButton("😠 Angry", 2) { viewModel.sendFaceExpression(2) }
            }

            Spacer(modifier = Modifier.height(12.dp))

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                ExpressionButton("😮 Surprised", 3) { viewModel.sendFaceExpression(3) }
                ExpressionButton("😐 Neutral", 4) { viewModel.sendFaceExpression(4) }
            }

            Spacer(modifier = Modifier.weight(1f))

            // Sensor data
            Card(modifier = Modifier.fillMaxWidth()) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Text("Sensors", style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.Bold)
                    Spacer(modifier = Modifier.height(4.dp))
                    Text("Temp: ${uiState.connectedRobot.temperature}°C | Humidity: ${uiState.connectedRobot.humidity}%")
                    Text("Obstacle: ${uiState.connectedRobot.obstacleDistanceCm}cm")
                    Text("IMU: pitch=${uiState.connectedRobot.imuPitch}° roll=${uiState.connectedRobot.imuRoll}°")
                }
            }
        }
    }
}

@Composable
fun StatusChip(label: String, value: String) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        Text(text = value, style = MaterialTheme.typography.bodyLarge, fontWeight = FontWeight.Bold)
        Text(text = label, style = MaterialTheme.typography.labelSmall)
    }
}

@Composable
fun DirectionPad(
    onForward: () -> Unit,
    onBackward: () -> Unit,
    onLeft: () -> Unit,
    onRight: () -> Unit,
    onStop: () -> Unit
) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        // Up
        Button(onClick = onForward, modifier = Modifier.size(64.dp)) {
            Text("▲", style = MaterialTheme.typography.titleLarge)
        }

        Row(verticalAlignment = Alignment.CenterVertically) {
            Button(onClick = onLeft, modifier = Modifier.size(64.dp)) {
                Text("◀", style = MaterialTheme.typography.titleLarge)
            }

            // Stop button
            Button(
                onClick = onStop,
                modifier = Modifier.size(64.dp),
                colors = ButtonDefaults.buttonColors(
                    containerColor = MaterialTheme.colorScheme.error
                )
            ) {
                Text("■", style = MaterialTheme.typography.titleLarge)
            }

            Button(onClick = onRight, modifier = Modifier.size(64.dp)) {
                Text("▶", style = MaterialTheme.typography.titleLarge)
            }
        }

        // Down
        Button(onClick = onBackward, modifier = Modifier.size(64.dp)) {
            Text("▼", style = MaterialTheme.typography.titleLarge)
        }
    }
}

@Composable
fun LedColorButton(label: String, color: Long, onClick: () -> Unit) {
    Button(
        onClick = onClick,
        modifier = Modifier.height(40.dp),
        colors = ButtonDefaults.buttonColors(
            containerColor = androidx.compose.ui.graphics.Color(color or 0xFF000000)
        )
    ) {
        Text(label, style = MaterialTheme.typography.labelSmall)
    }
}

@Composable
fun ExpressionButton(label: String, expr: Int, onClick: () -> Unit) {
    OutlinedButton(onClick = onClick, modifier = Modifier.height(40.dp)) {
        Text(label, style = MaterialTheme.typography.labelSmall)
    }
}
