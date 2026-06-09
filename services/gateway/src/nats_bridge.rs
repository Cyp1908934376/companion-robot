//! NATS message bridge — connects the gateway to the main-brain message bus.
//!
//! Subject routing:
//!   Uplink (robot → main brain):
//!     robot.{short_id}.heartbeat  — heartbeat data
//!     robot.{short_id}.sensor     — sensor data (env, IMU, touch, obstacle)
//!     robot.{short_id}.event      — events (motion, audio, image)
//!     robot.{short_id}.task_status — task progress updates
//!     robot.{short_id}.response   — responses to commands
//!
//!   Downlink (main brain → robot):
//!     robot.{short_id}.cmd        — commands to execute
//!     robot.{short_id}.task       — task assignments
//!     robot.{short_id}.config     — configuration updates
//!     robot.{short_id}.ota        — OTA updates
//!     broadcast.emergency         — emergency broadcast to all robots

use async_nats::{Client, Subject};
use bcp_core::{BcpFrame, Command};
use futures_util::StreamExt;
use tokio::sync::mpsc;

use crate::error::{GatewayError, Result};

/// NATS bridge wrapping an async_nats client.
#[derive(Clone)]
pub struct NatsBridge {
    client: Client,
}

impl NatsBridge {
    /// Connect to a NATS server.
    pub async fn connect(url: &str) -> Result<Self> {
        let client = async_nats::connect(url)
            .await
            .map_err(|e| GatewayError::NatsError(e.to_string()))?;
        tracing::info!(url, "connected to NATS");
        Ok(NatsBridge { client })
    }

    /// Publish a BCP frame's commands to the appropriate NATS subjects.
    pub async fn publish_frame(&self, short_id: u16, frame: &BcpFrame) -> Result<()> {
        for cmd in &frame.commands {
            let subject = uplink_subject(short_id, cmd);
            let payload = serde_json::to_vec(&CmdPayload::from(cmd))?;
            self.client
                .publish(Subject::from(subject), payload.into())
                .await
                .map_err(|e| GatewayError::NatsError(e.to_string()))?;
        }
        Ok(())
    }

    /// Subscribe to downlink commands for a specific robot.
    pub async fn subscribe_robot(&self, short_id: u16) -> Result<mpsc::Receiver<BcpFrame>> {
        let subject = format!("robot.{}.cmd", short_id);
        let mut sub = self
            .client
            .subscribe(Subject::from(subject))
            .await
            .map_err(|e| GatewayError::NatsError(e.to_string()))?;

        let (tx, rx) = mpsc::channel::<BcpFrame>(64);
        let _client = self.client.clone();

        tokio::spawn(async move {
            while let Some(msg) = sub.next().await {
                match serde_json::from_slice::<CmdPayload>(&msg.payload) {
                    Ok(cmd_payload) => {
                        if let Some(frame) = cmd_payload.into_frame() {
                            if tx.send(frame).await.is_err() {
                                break;
                            }
                        }
                    }
                    Err(e) => {
                        tracing::warn!(short_id, error = %e, "failed to parse NATS message");
                    }
                }
            }
        });

        Ok(rx)
    }

    /// Publish to a raw subject.
    #[allow(dead_code)]
    pub async fn publish_raw(&self, subject: &str, payload: Vec<u8>) -> Result<()> {
        self.client
            .publish(Subject::from(subject.to_string()), payload.into())
            .await
            .map_err(|e| GatewayError::NatsError(e.to_string()))?;
        Ok(())
    }

    /// Subscribe to broadcast emergency commands for all robots.
    pub async fn subscribe_broadcast(&self) -> Result<mpsc::Receiver<BcpFrame>> {
        let mut sub = self
            .client
            .subscribe(Subject::from("broadcast.emergency"))
            .await
            .map_err(|e| GatewayError::NatsError(e.to_string()))?;

        let (tx, rx) = mpsc::channel::<BcpFrame>(32);

        tokio::spawn(async move {
            while let Some(msg) = sub.next().await {
                match serde_json::from_slice::<CmdPayload>(&msg.payload) {
                    Ok(cmd_payload) => {
                        if let Some(frame) = cmd_payload.into_frame() {
                            if tx.send(frame).await.is_err() {
                                break;
                            }
                        }
                    }
                    Err(_) => {}
                }
            }
        });

        Ok(rx)
    }
}

/// Serializable command payload for NATS messages.
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct CmdPayload {
    pub cmd: String,
    #[serde(default)]
    pub params: serde_json::Value,
}

impl From<&Command> for CmdPayload {
    fn from(cmd: &Command) -> Self {
        let (cmd_name, params) = match cmd {
            Command::Heartbeat { status, battery, rssi, task_id } => (
                "heartbeat",
                serde_json::json!({ "status": status, "battery": battery, "rssi": rssi, "task_id": task_id }),
            ),
            Command::EnvData { temp, humi, pressure, light, air } => (
                "env_data",
                serde_json::json!({ "temp": temp, "humi": humi, "pressure": pressure, "light": light, "air": air }),
            ),
            Command::MotionEvent { detect_type, confidence } => (
                "motion_event",
                serde_json::json!({ "detect_type": detect_type, "confidence": confidence }),
            ),
            Command::AudioEvent { event_type, energy } => (
                "audio_event",
                serde_json::json!({ "event_type": event_type, "energy": energy }),
            ),
            Command::TouchEvent { zone, pressure, state } => (
                "touch_event",
                serde_json::json!({ "zone": zone, "pressure": pressure, "state": state }),
            ),
            Command::Obstacle { direction, distance } => (
                "obstacle",
                serde_json::json!({ "direction": direction, "distance": distance }),
            ),
            Command::ImuData { ax, ay, az, gx, gy, gz } => (
                "imu_data",
                serde_json::json!({ "ax": ax, "ay": ay, "az": az, "gx": gx, "gy": gy, "gz": gz }),
            ),
            Command::TaskStatus { task_id, status, progress } => (
                "task_status",
                serde_json::json!({ "task_id": task_id, "status": status, "progress": progress }),
            ),
            _ => ("unknown", serde_json::json!({})),
        };
        CmdPayload { cmd: cmd_name.into(), params }
    }
}

impl CmdPayload {
    /// Convert a NATS command payload back into a BcpFrame (for downlink).
    fn into_frame(&self) -> Option<BcpFrame> {
        let cmd = match self.cmd.as_str() {
            "stop" => {
                let emergency = self.params.get("emergency").and_then(|v| v.as_bool()).unwrap_or(false);
                Command::Stop { emergency }
            }
            "move" => {
                let dir = self.params.get("direction").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let speed = self.params.get("speed").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let direction = bcp_core::types::Direction::from_u8(dir)?;
                Command::Move { direction, speed }
            }
            "led_pattern" => {
                let mode = self.params.get("mode").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let speed = self.params.get("speed").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let r = self.params.get("r").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let g = self.params.get("g").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let b = self.params.get("b").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let mode = bcp_core::types::LedMode::from_u8(mode)?;
                Command::LedPattern { mode, speed, r, g, b }
            }
            "face_expr" => {
                let expr = self.params.get("expr").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                let expr = bcp_core::types::FaceExpression::from_u8(expr)?;
                Command::FaceExpr { expr }
            }
            "speak" => {
                let volume = self.params.get("volume").and_then(|v| v.as_u64()).unwrap_or(50) as u8;
                let data = heapless::Vec::new();
                Command::Speak { volume, format: bcp_core::types::AudioFormat::Opus, data }
            }
            "reset" => {
                let reason_code = self.params.get("reason_code").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                Command::Reset { reason_code }
            }
            "reg_ack" => {
                let short_id = self.params.get("short_id").and_then(|v| v.as_u64()).unwrap_or(0) as u16;
                let heartbeat_interval = self.params.get("heartbeat_interval").and_then(|v| v.as_u64()).unwrap_or(5000) as u16;
                Command::RegAck { short_id, heartbeat_interval }
            }
            _ => {
                tracing::debug!(cmd = %self.cmd, "unknown downlink command");
                return None;
            }
        };

        let mut frame = BcpFrame::new(0);
        frame.push(cmd).ok()?;
        Some(frame)
    }
}

/// Determine the NATS subject for an uplink command.
fn uplink_subject(short_id: u16, cmd: &Command) -> String {
    let suffix = match cmd {
        Command::Heartbeat { .. } => "heartbeat",
        Command::EnvData { .. }
        | Command::ImuData { .. }
        | Command::TouchEvent { .. }
        | Command::Obstacle { .. } => "sensor",
        Command::MotionEvent { .. }
        | Command::AudioEvent { .. }
        | Command::AudioStream { .. }
        | Command::ImageSnapshot { .. } => "event",
        Command::TaskStatus { .. } | Command::TaskCancel { .. } => "task_status",
        _ => "event",
    };
    format!("robot.{}.{}", short_id, suffix)
}
