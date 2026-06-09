/// Task model — the core scheduling unit.
///
/// Tasks flow:
///   Pending → (assigned) → Running → Completed
///                                  → Failed → (retry) → Pending
///                                  → Cancelled
///
/// Priority: 0 (Emergency) → 255 (Background)

use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};
use sqlx::FromRow;

// ── Task type ──────────────────────────────────────────────────

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum TaskType {
    #[serde(rename = "patrol")]
    Patrol,
    #[serde(rename = "dialogue")]
    Dialogue,
    #[serde(rename = "transport")]
    Transport,
    #[serde(rename = "charge")]
    Charge,
    #[serde(rename = "formation")]
    Formation,
    #[serde(rename = "custom")]
    Custom,
}

impl TaskType {
    pub fn as_str(&self) -> &'static str {
        match self {
            TaskType::Patrol => "patrol",
            TaskType::Dialogue => "dialogue",
            TaskType::Transport => "transport",
            TaskType::Charge => "charge",
            TaskType::Formation => "formation",
            TaskType::Custom => "custom",
        }
    }

    pub fn from_str(s: &str) -> Option<Self> {
        match s {
            "patrol" => Some(TaskType::Patrol),
            "dialogue" => Some(TaskType::Dialogue),
            "transport" => Some(TaskType::Transport),
            "charge" => Some(TaskType::Charge),
            "formation" => Some(TaskType::Formation),
            "custom" => Some(TaskType::Custom),
            _ => None,
        }
    }

    /// Capabilities required for this task type.
    pub fn required_capabilities(&self) -> &[&str] {
        match self {
            TaskType::Patrol => &["movement"],
            TaskType::Dialogue => &["movement", "audio", "expression"],
            TaskType::Transport => &["movement", "charging"],
            TaskType::Charge => &["movement", "charging"],
            TaskType::Formation => &["movement", "swarm"],
            TaskType::Custom => &["movement"],
        }
    }
}

// ── Task status ────────────────────────────────────────────────

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum TaskStatus {
    #[serde(rename = "pending")]
    Pending,
    #[serde(rename = "running")]
    Running,
    #[serde(rename = "completed")]
    Completed,
    #[serde(rename = "failed")]
    Failed,
    #[serde(rename = "cancelled")]
    Cancelled,
}

impl TaskStatus {
    pub fn as_i16(&self) -> i16 {
        match self {
            TaskStatus::Pending => 0,
            TaskStatus::Running => 1,
            TaskStatus::Completed => 2,
            TaskStatus::Failed => 3,
            TaskStatus::Cancelled => 4,
        }
    }

    pub fn from_i16(v: i16) -> Option<Self> {
        match v {
            0 => Some(TaskStatus::Pending),
            1 => Some(TaskStatus::Running),
            2 => Some(TaskStatus::Completed),
            3 => Some(TaskStatus::Failed),
            4 => Some(TaskStatus::Cancelled),
            _ => None,
        }
    }
}

// ── Task ───────────────────────────────────────────────────────

#[derive(Debug, Clone, FromRow, Serialize, Deserialize)]
pub struct Task {
    pub id: i64,
    pub task_type: String,
    pub priority: i16,
    pub target_robot_short_id: Option<i16>,
    pub preferred_robot_short_id: Option<i16>,
    pub status: i16,
    pub params: serde_json::Value,
    pub target_x_cm: Option<i32>,
    pub target_y_cm: Option<i32>,
    pub created_at: DateTime<Utc>,
    pub started_at: Option<DateTime<Utc>>,
    pub completed_at: Option<DateTime<Utc>>,
    pub timeout_secs: i32,
    pub retry_count: i16,
    pub max_retries: i16,
}

impl Task {
    pub fn status_enum(&self) -> Option<TaskStatus> {
        TaskStatus::from_i16(self.status)
    }

    pub fn is_terminal(&self) -> bool {
        matches!(
            self.status_enum(),
            Some(TaskStatus::Completed) | Some(TaskStatus::Failed) | Some(TaskStatus::Cancelled)
        )
    }

    pub fn can_retry(&self) -> bool {
        self.retry_count < self.max_retries
    }
}

// ── API request/response types ─────────────────────────────────

#[derive(Debug, Deserialize)]
pub struct CreateTaskRequest {
    pub task_type: String,
    pub priority: Option<i16>,
    pub target_robot_short_id: Option<i16>,
    pub preferred_robot_short_id: Option<i16>,
    pub params: Option<serde_json::Value>,
    pub target_x_cm: Option<i32>,
    pub target_y_cm: Option<i32>,
    pub timeout_secs: Option<i32>,
    pub max_retries: Option<i32>,
}

#[derive(Debug, Serialize)]
pub struct TaskListResponse {
    pub tasks: Vec<Task>,
    pub count: usize,
}

// ── Robot info for assignment (from device-manager) ────────────

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RobotInfo {
    pub id: i64,
    pub short_id: i16,
    pub machine_id_hex: String,
    pub name: String,
    pub capabilities: serde_json::Value,
    pub status: i16,
    pub battery: Option<i16>,
    pub x_cm: Option<i32>,
    pub y_cm: Option<i32>,
    pub heading: Option<i16>,
    pub active_tasks: i32,
}
