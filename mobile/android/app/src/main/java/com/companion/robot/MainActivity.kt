package com.companion.robot

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.ui.Modifier
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import com.companion.robot.ui.screens.DeviceListScreen
import com.companion.robot.ui.screens.DialogueScreen
import com.companion.robot.ui.screens.RobotControlScreen
import com.companion.robot.ui.theme.CompanionRobotTheme
import com.companion.robot.viewmodel.RobotViewModel

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            CompanionRobotTheme {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background
                ) {
                    val navController = rememberNavController()
                    val viewModel: RobotViewModel = viewModel()

                    NavHost(navController, startDestination = "devices") {
                        composable("devices") {
                            DeviceListScreen(
                                viewModel = viewModel,
                                onDeviceSelected = { deviceId ->
                                    navController.navigate("control/$deviceId")
                                }
                            )
                        }
                        composable("control/{deviceId}") { backStackEntry ->
                            val deviceId = backStackEntry.arguments?.getString("deviceId") ?: ""
                            RobotControlScreen(
                                viewModel = viewModel,
                                deviceId = deviceId,
                                onBack = { navController.popBackStack() },
                                onDialogue = { navController.navigate("dialogue/$deviceId") }
                            )
                        }
                        composable("dialogue/{deviceId}") { backStackEntry ->
                            val deviceId = backStackEntry.arguments?.getString("deviceId") ?: ""
                            DialogueScreen(
                                viewModel = viewModel,
                                deviceId = deviceId,
                                onBack = { navController.popBackStack() }
                            )
                        }
                    }
                }
            }
        }
    }
}
