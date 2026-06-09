//! Command enum covering all BCP instruction types.
//!
//! Organized by group:
//! - System (0x00xx): heartbeat, registration, ping, OTA, error
//! - Motion (0x01xx): move, stop, servo, head
//! - Expression (0x02xx): LED, face, audio, TTS
//! - Perception (0x03xx): environment, audio, vision, touch, IMU, obstacle
//! - Cluster (0x04xx): task, swarm, peer messaging

use crate::constants::*;
use crate::types::*;

/// All BCP protocol commands.
///
/// Each variant maps to a specific CmdID and carries typed payload fields
/// matching the protocol specification in `docs/protocol.md`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Command {
    // ── System (0x00xx) ──────────────────────────────
    /// Heartbeat from robot to main brain.
    Heartbeat {
        status: u8,
        battery: u8,
        rssi: i8,
        task_id: u16,
    },
    /// Robot registration request.
    Register {
        capabilities: Capabilities,
        firmware_version: [u8; 4],
    },
    /// Registration acknowledgment.
    RegAck {
        short_id: u16,
        heartbeat_interval: u16,
    },
    /// Latency probe request.
    Ping {
        timestamp: u32,
    },
    /// Latency probe response.
    Pong {
        timestamp: u32,
    },
    /// Remote reset command.
    Reset {
        reason_code: u8,
    },
    /// OTA firmware update start.
    OtaStart {
        size: u32,
        md5: [u8; 16],
    },
    /// OTA firmware chunk.
    OtaChunk {
        offset: u32,
        data: heapless::Vec<u8, 255>,
    },
    /// OTA firmware transfer complete.
    OtaDone,
    /// Protocol error report.
    Error {
        error_code: u16,
        level: u8,
        related_seq_no: u16,
    },

    // ── Motion (0x01xx) ──────────────────────────────
    /// Directional movement.
    Move {
        direction: Direction,
        speed: u8,
    },
    /// Coordinate-based movement (relative, cm).
    MoveTo {
        x: i16,
        y: i16,
        speed: u8,
    },
    /// Stop movement.
    Stop {
        emergency: bool,
    },
    /// Single servo angle set.
    ServoSet {
        id: u8,
        angle: u16,
    },
    /// Batch servo control.
    ServoBatch {
        servos: heapless::Vec<(u8, u16), 32>,
    },
    /// Head pan/tilt control.
    HeadPanTilt {
        pan: u8,
        tilt: u8,
    },

    // ── Expression (0x02xx) ──────────────────────────
    /// Single LED color set.
    LedSet {
        id: u16,
        r: u8,
        g: u8,
        b: u8,
    },
    /// LED animation pattern.
    LedPattern {
        mode: LedMode,
        speed: u8,
        r: u8,
        g: u8,
        b: u8,
    },
    /// Turn off all LEDs.
    LedOff,
    /// Preset facial expression.
    FaceExpr {
        expr: FaceExpression,
    },
    /// Custom face display frame.
    FaceCustom {
        frame_data: heapless::Vec<u8, 255>,
    },
    /// Play audio sample.
    Speak {
        volume: u8,
        format: AudioFormat,
        data: heapless::Vec<u8, 255>,
    },
    /// TTS text (main brain synthesizes).
    TtsText {
        encoding: u8,
        text: heapless::Vec<u8, 255>,
    },

    // ── Perception (0x03xx) ──────────────────────────
    /// Environmental sensor data.
    EnvData {
        temp: i16,
        humi: u16,
        pressure: u32,
        light: u32,
        air: u16,
    },
    /// Motion detection event.
    MotionEvent {
        detect_type: u8,
        confidence: u8,
    },
    /// Audio event (VAD start/end, wake-word).
    AudioEvent {
        event_type: u8,
        energy: u16,
    },
    /// Streaming audio data.
    AudioStream {
        encoding: u8,
        data: heapless::Vec<u8, 255>,
    },
    /// JPEG image snapshot.
    ImageSnapshot {
        format: ImageFormat,
        width: u16,
        height: u16,
        data: heapless::Vec<u8, 255>,
    },
    /// Depth map data.
    DepthData {
        width: u16,
        height: u16,
        data: heapless::Vec<u8, 255>,
    },
    /// Capacitive touch event.
    TouchEvent {
        zone: u8,
        pressure: u8,
        state: u8,
    },
    /// Raw IMU data (accelerometer + gyroscope).
    ImuData {
        ax: i16,
        ay: i16,
        az: i16,
        gx: i16,
        gy: i16,
        gz: i16,
    },
    /// Obstacle detection report.
    Obstacle {
        direction: u8,
        distance: u16,
    },

    // ── Cluster (0x04xx) ─────────────────────────────
    /// Task assignment from scheduler.
    TaskAssign {
        task_type: u8,
        priority: u8,
        params: heapless::Vec<u8, 255>,
    },
    /// Task progress update.
    TaskStatus {
        task_id: u16,
        status: u8,
        progress: u8,
    },
    /// Task cancellation.
    TaskCancel {
        task_id: u16,
    },
    /// Swarm formation command.
    SwarmForm {
        formation_id: u8,
        coordinates: heapless::Vec<u8, 255>,
    },
    /// Inter-robot peer message.
    PeerMsg {
        target: u16,
        data: heapless::Vec<u8, 255>,
    },
}

impl Command {
    /// Return the on-wire command ID for this variant.
    pub fn cmd_id(&self) -> u16 {
        match self {
            // System
            Command::Heartbeat { .. } => CMD_HEARTBEAT,
            Command::Register { .. } => CMD_REGISTER,
            Command::RegAck { .. } => CMD_REG_ACK,
            Command::Ping { .. } => CMD_PING,
            Command::Pong { .. } => CMD_PONG,
            Command::Reset { .. } => CMD_RESET,
            Command::OtaStart { .. } => CMD_OTA_START,
            Command::OtaChunk { .. } => CMD_OTA_CHUNK,
            Command::OtaDone => CMD_OTA_DONE,
            Command::Error { .. } => CMD_ERROR,
            // Motion
            Command::Move { .. } => CMD_MOVE,
            Command::MoveTo { .. } => CMD_MOVE_TO,
            Command::Stop { .. } => CMD_STOP,
            Command::ServoSet { .. } => CMD_SERVO_SET,
            Command::ServoBatch { .. } => CMD_SERVO_BATCH,
            Command::HeadPanTilt { .. } => CMD_HEAD_PAN_TILT,
            // Expression
            Command::LedSet { .. } => CMD_LED_SET,
            Command::LedPattern { .. } => CMD_LED_PATTERN,
            Command::LedOff => CMD_LED_OFF,
            Command::FaceExpr { .. } => CMD_FACE_EXPR,
            Command::FaceCustom { .. } => CMD_FACE_CUSTOM,
            Command::Speak { .. } => CMD_SPEAK,
            Command::TtsText { .. } => CMD_TTS_TEXT,
            // Perception
            Command::EnvData { .. } => CMD_ENV_DATA,
            Command::MotionEvent { .. } => CMD_MOTION_EVENT,
            Command::AudioEvent { .. } => CMD_AUDIO_EVENT,
            Command::AudioStream { .. } => CMD_AUDIO_STREAM,
            Command::ImageSnapshot { .. } => CMD_IMAGE_SNAPSHOT,
            Command::DepthData { .. } => CMD_DEPTH_DATA,
            Command::TouchEvent { .. } => CMD_TOUCH_EVENT,
            Command::ImuData { .. } => CMD_IMU_DATA,
            Command::Obstacle { .. } => CMD_OBSTACLE,
            // Cluster
            Command::TaskAssign { .. } => CMD_TASK_ASSIGN,
            Command::TaskStatus { .. } => CMD_TASK_STATUS,
            Command::TaskCancel { .. } => CMD_TASK_CANCEL,
            Command::SwarmForm { .. } => CMD_SWARM_FORM,
            Command::PeerMsg { .. } => CMD_PEER_MSG,
        }
    }

    /// Return the payload length in bytes for this command on the wire.
    pub fn payload_len(&self) -> usize {
        match self {
            Command::Heartbeat { .. } => 5,
            Command::Register { .. } => 2 + 4, // capabilities(2) + fw_ver(4)
            Command::RegAck { .. } => 4,
            Command::Ping { .. } | Command::Pong { .. } => 4,
            Command::Reset { .. } => 1,
            Command::OtaStart { .. } => 20,
            Command::OtaChunk { data, .. } => 4 + data.len(),
            Command::OtaDone => 0,
            Command::Error { .. } => 5, // error_code(2) + level(1) + related_seq_no(2)
            Command::Move { .. } => 2,
            Command::MoveTo { .. } => 5,
            Command::Stop { .. } => 1,
            Command::ServoSet { .. } => 3,
            Command::ServoBatch { servos } => 1 + servos.len() * 3,
            Command::HeadPanTilt { .. } => 2,
            Command::LedSet { .. } => 5,
            Command::LedPattern { .. } => 5,
            Command::LedOff => 0,
            Command::FaceExpr { .. } => 1,
            Command::FaceCustom { frame_data } => frame_data.len(),
            Command::Speak { data, .. } => 2 + data.len(),
            Command::TtsText { text, .. } => 1 + text.len(),
            Command::EnvData { .. } => 14,
            Command::MotionEvent { .. } => 2,
            Command::AudioEvent { .. } => 3,
            Command::AudioStream { data, .. } => 1 + data.len(),
            Command::ImageSnapshot { data, .. } => 5 + data.len(),
            Command::DepthData { data, .. } => 4 + data.len(),
            Command::TouchEvent { .. } => 3,
            Command::ImuData { .. } => 12,
            Command::Obstacle { .. } => 3,
            Command::TaskAssign { params, .. } => 2 + params.len(),
            Command::TaskStatus { .. } => 4,
            Command::TaskCancel { .. } => 2,
            Command::SwarmForm { coordinates, .. } => 1 + coordinates.len(),
            Command::PeerMsg { data, .. } => 2 + data.len(),
        }
    }

    /// Total size on the wire: 3-byte command header + payload.
    pub fn wire_len(&self) -> usize {
        CMD_HEADER_LEN + self.payload_len()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_cmd_id_unique() {
        // Verify that different command groups have non-overlapping IDs
        use std::collections::HashSet;
        let mut ids = HashSet::new();

        let commands: &[Command] = &[
            Command::Heartbeat { status: 0, battery: 100, rssi: -40, task_id: 0 },
            Command::Register { capabilities: Capabilities::new(), firmware_version: [1, 0, 0, 0] },
            Command::RegAck { short_id: 1, heartbeat_interval: 5000 },
            Command::Ping { timestamp: 0 },
            Command::Pong { timestamp: 0 },
            Command::Reset { reason_code: 0 },
            Command::OtaStart { size: 0, md5: [0; 16] },
            Command::OtaDone,
            Command::Move { direction: Direction::Forward, speed: 0 },
            Command::MoveTo { x: 0, y: 0, speed: 0 },
            Command::Stop { emergency: false },
            Command::ServoSet { id: 0, angle: 90 },
            Command::LedSet { id: 0, r: 0, g: 0, b: 0 },
            Command::LedPattern { mode: LedMode::Breathing, speed: 0, r: 0, g: 0, b: 0 },
            Command::LedOff,
            Command::FaceExpr { expr: FaceExpression::Neutral },
            Command::EnvData { temp: 25, humi: 50, pressure: 101325, light: 500, air: 400 },
            Command::MotionEvent { detect_type: 1, confidence: 80 },
            Command::AudioEvent { event_type: 0, energy: 100 },
            Command::TouchEvent { zone: 0, pressure: 0, state: 0 },
            Command::Obstacle { direction: 0, distance: 100 },
            Command::TaskAssign { task_type: 1, priority: 0, params: heapless::Vec::new() },
            Command::TaskStatus { task_id: 1, status: 0, progress: 50 },
            Command::TaskCancel { task_id: 1 },
        ];

        for cmd in commands {
            let id = cmd.cmd_id();
            assert!(ids.insert(id), "Duplicate CmdID: 0x{:04X}", id);
        }
    }
}
