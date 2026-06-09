//! Shared types for the BCP protocol.

/// Movement direction for the MOVE command.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum Direction {
    Forward = 0,
    Backward = 1,
    Left = 2,
    Right = 3,
    RotateLeft = 4,
    RotateRight = 5,
}

impl Direction {
    pub fn from_u8(v: u8) -> Option<Self> {
        match v {
            0 => Some(Direction::Forward),
            1 => Some(Direction::Backward),
            2 => Some(Direction::Left),
            3 => Some(Direction::Right),
            4 => Some(Direction::RotateLeft),
            5 => Some(Direction::RotateRight),
            _ => None,
        }
    }
}

/// Audio encoding format.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum AudioFormat {
    Pcm = 0,
    Opus = 1,
}

impl AudioFormat {
    pub fn from_u8(v: u8) -> Option<Self> {
        match v {
            0 => Some(AudioFormat::Pcm),
            1 => Some(AudioFormat::Opus),
            _ => None,
        }
    }
}

/// Image encoding format.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum ImageFormat {
    Jpeg = 0,
    Raw = 1,
}

impl ImageFormat {
    pub fn from_u8(v: u8) -> Option<Self> {
        match v {
            0 => Some(ImageFormat::Jpeg),
            1 => Some(ImageFormat::Raw),
            _ => None,
        }
    }
}

/// Robot capability flags (bitmask).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Capabilities(u16);

impl Capabilities {
    pub const MOVEMENT: u16 = 1 << 0;
    pub const AUDIO: u16 = 1 << 1;
    pub const VISION: u16 = 1 << 2;
    pub const TOUCH: u16 = 1 << 3;
    pub const EXPRESSION: u16 = 1 << 4;
    pub const SWARM: u16 = 1 << 5;
    pub const CHARGING: u16 = 1 << 6;

    pub const fn new() -> Self {
        Capabilities(0)
    }

    pub const fn from_bits(bits: u16) -> Self {
        Capabilities(bits)
    }

    pub const fn bits(&self) -> u16 {
        self.0
    }

    pub fn has(&self, flag: u16) -> bool {
        (self.0 & flag) != 0
    }

    pub fn set(&mut self, flag: u16) {
        self.0 |= flag;
    }
}

/// Command priority levels for frame packing.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
#[repr(u8)]
pub enum Priority {
    /// Emergency/safety commands — send immediately, bypass queue.
    Emergency = 0,
    /// Motion control — target <10ms latency.
    Motion = 1,
    /// Expression (LED, face, audio) — target <50ms latency.
    Expression = 2,
    /// Best-effort (sensor data, OTA chunks).
    BestEffort = 3,
}

/// LED pattern animation mode.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum LedMode {
    Breathing = 0,
    Flow = 1,
    Blink = 2,
    Rainbow = 3,
}

impl LedMode {
    pub fn from_u8(v: u8) -> Option<Self> {
        match v {
            0 => Some(LedMode::Breathing),
            1 => Some(LedMode::Flow),
            2 => Some(LedMode::Blink),
            3 => Some(LedMode::Rainbow),
            _ => None,
        }
    }
}

/// Facial expression presets.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum FaceExpression {
    Neutral = 0,
    Happy = 1,
    Sad = 2,
    Surprised = 3,
    Angry = 4,
    Confused = 5,
}

impl FaceExpression {
    pub fn from_u8(v: u8) -> Option<Self> {
        match v {
            0 => Some(FaceExpression::Neutral),
            1 => Some(FaceExpression::Happy),
            2 => Some(FaceExpression::Sad),
            3 => Some(FaceExpression::Surprised),
            4 => Some(FaceExpression::Angry),
            5 => Some(FaceExpression::Confused),
            _ => None,
        }
    }
}

/// Full heartbeat state for delta comparison.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct HeartbeatState {
    pub short_id: u16,
    pub status: u8,
    pub battery: u8,
    pub rssi: i8,
    pub task_id: u16,
}

/// Delta-encoded heartbeat variants for bandwidth optimization.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum HeartbeatDelta {
    /// No fields changed — 3 bytes: [short_id, 0xFF]
    NoChange { short_id: u16 },
    /// Only battery changed — 3 bytes: [short_id, battery]
    BatteryOnly { short_id: u16, battery: u8 },
    /// Multiple fields changed — full 7-byte state.
    Full(HeartbeatState),
}

/// Status flags bit definitions for heartbeat.
pub mod status_flags {
    pub const MOVING: u8 = 1 << 0;
    pub const CHARGING: u8 = 1 << 1;
    pub const LOW_BATTERY: u8 = 1 << 2;
    pub const SENSOR_FAULT: u8 = 1 << 3;
    pub const COMM_ERROR: u8 = 1 << 4;
}

/// Stop type.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum StopType {
    Soft = 0,
    Emergency = 1,
}

/// Task status values.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum TaskStatus {
    Pending = 0,
    InProgress = 1,
    Completed = 2,
    Failed = 3,
    Cancelled = 4,
}
