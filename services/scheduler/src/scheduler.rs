/// Task scheduling engine — weighted scoring assignment algorithm.
///
/// Flow:
///   1. Receive new task (Pending)
///   2. Query available robots from device-manager (Redis cache + PostgreSQL)
///   3. Filter by: online, battery > 20%, has required capabilities
///   4. Score each candidate by weighted criteria
///   5. Assign to best-scoring robot
///   6. Dispatch task command via NATS
///   7. Monitor task progress, handle retries/timeouts

use chrono::Utc;
use sqlx::postgres::PgPool;

use crate::config::Config;
use crate::error::Error;
use crate::metrics;
use crate::task::{RobotInfo, Task, TaskStatus, TaskType};

// ── Scoring weights ────────────────────────────────────────────

const WEIGHT_DISTANCE: f64 = 0.30;
const WEIGHT_LOAD: f64 = 0.20;
const WEIGHT_BATTERY: f64 = 0.20;
const WEIGHT_AFFINITY: f64 = 0.30;

// ── DB queries ─────────────────────────────────────────────────

pub async fn init_db(pool: &PgPool) -> Result<(), Error> {
    sqlx::query(
        r#"
        CREATE TABLE IF NOT EXISTS tasks (
            id                      BIGSERIAL PRIMARY KEY,
            task_type               VARCHAR(32) NOT NULL,
            priority                SMALLINT NOT NULL DEFAULT 128,
            target_robot_short_id   SMALLINT,
            preferred_robot_short_id SMALLINT,
            status                  SMALLINT NOT NULL DEFAULT 0,
            params                  JSONB NOT NULL DEFAULT '{}',
            target_x_cm             INT,
            target_y_cm             INT,
            created_at              TIMESTAMPTZ NOT NULL DEFAULT NOW(),
            started_at              TIMESTAMPTZ,
            completed_at            TIMESTAMPTZ,
            timeout_secs            INT NOT NULL DEFAULT 30,
            retry_count             SMALLINT NOT NULL DEFAULT 0,
            max_retries             SMALLINT NOT NULL DEFAULT 3
        );

        CREATE INDEX IF NOT EXISTS idx_tasks_status ON tasks(status);
        CREATE INDEX IF NOT EXISTS idx_tasks_robot ON tasks(target_robot_short_id);
        CREATE INDEX IF NOT EXISTS idx_tasks_priority ON tasks(priority, created_at);
        "#
    )
    .execute(pool)
    .await?;

    tracing::info!("tasks table initialized");
    Ok(())
}

pub async fn create_task(pool: &PgPool, req: &crate::task::CreateTaskRequest) -> Result<Task, Error> {
    let task_type = TaskType::from_str(&req.task_type)
        .ok_or_else(|| Error::InvalidInput(format!("unknown task_type: {}", req.task_type)))?;

    let task = sqlx::query_as::<_, Task>(
        r#"INSERT INTO tasks
           (task_type, priority, target_robot_short_id, preferred_robot_short_id,
            params, target_x_cm, target_y_cm, timeout_secs, max_retries)
           VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)
           RETURNING *"#
    )
    .bind(task_type.as_str())
    .bind(req.priority.unwrap_or(128))
    .bind(req.target_robot_short_id)
    .bind(req.preferred_robot_short_id)
    .bind(req.params.clone().unwrap_or(serde_json::json!({})))
    .bind(req.target_x_cm)
    .bind(req.target_y_cm)
    .bind(req.timeout_secs.unwrap_or(30))
    .bind(req.max_retries.unwrap_or(3) as i16)
    .fetch_one(pool)
    .await?;

    metrics::TASKS_CREATED.inc();
    tracing::info!("task created: id={} type={}", task.id, task.task_type);

    Ok(task)
}

pub async fn list_tasks(pool: &PgPool) -> Result<Vec<Task>, Error> {
    Ok(sqlx::query_as::<_, Task>("SELECT * FROM tasks ORDER BY priority, created_at")
        .fetch_all(pool)
        .await?)
}

pub async fn list_pending_tasks(pool: &PgPool) -> Result<Vec<Task>, Error> {
    Ok(sqlx::query_as::<_, Task>(
        "SELECT * FROM tasks WHERE status = 0 ORDER BY priority, created_at"
    )
    .fetch_all(pool)
    .await?)
}

pub async fn get_task(pool: &PgPool, id: i64) -> Result<Option<Task>, Error> {
    Ok(sqlx::query_as::<_, Task>("SELECT * FROM tasks WHERE id = $1")
        .bind(id)
        .fetch_optional(pool)
        .await?)
}

pub async fn update_task_status(
    pool: &PgPool,
    id: i64,
    status: TaskStatus,
) -> Result<Task, Error> {
    let now = Utc::now();

    let task = match status {
        TaskStatus::Running => {
            sqlx::query_as::<_, Task>(
                "UPDATE tasks SET status=$1, started_at=$2 WHERE id=$3 RETURNING *"
            )
            .bind(status.as_i16())
            .bind(now)
            .bind(id)
            .fetch_optional(pool)
            .await?
        }
        TaskStatus::Completed | TaskStatus::Failed | TaskStatus::Cancelled => {
            sqlx::query_as::<_, Task>(
                "UPDATE tasks SET status=$1, completed_at=$2 WHERE id=$3 RETURNING *"
            )
            .bind(status.as_i16())
            .bind(now)
            .bind(id)
            .fetch_optional(pool)
            .await?
        }
        _ => {
            sqlx::query_as::<_, Task>(
                "UPDATE tasks SET status=$1 WHERE id=$2 RETURNING *"
            )
            .bind(status.as_i16())
            .bind(id)
            .fetch_optional(pool)
            .await?
        }
    };

    task.ok_or_else(|| Error::NotFound(format!("task id={}", id)))
}

pub async fn assign_robot(pool: &PgPool, id: i64, short_id: i16) -> Result<Task, Error> {
    let task = sqlx::query_as::<_, Task>(
        "UPDATE tasks SET target_robot_short_id=$1 WHERE id=$2 RETURNING *"
    )
    .bind(short_id)
    .bind(id)
    .fetch_optional(pool)
    .await?;

    task.ok_or_else(|| Error::NotFound(format!("task id={}", id)))
}

pub async fn increment_retry(pool: &PgPool, id: i64) -> Result<Task, Error> {
    let task = sqlx::query_as::<_, Task>(
        "UPDATE tasks SET retry_count=retry_count+1, status=0 WHERE id=$1 RETURNING *"
    )
    .bind(id)
    .fetch_optional(pool)
    .await?;

    task.ok_or_else(|| Error::NotFound(format!("task id={}", id)))
}

// ── Assignment algorithm ───────────────────────────────────────

/// Score a robot candidate for a given task.
/// Lower score = better fit.
pub fn score_robot(robot: &RobotInfo, task: &Task) -> f64 {
    // Distance: normalize to 0–1 (assuming max 5000cm range)
    let distance_score = if let (Some(tx), Some(ty), Some(rx), Some(ry)) =
        (task.target_x_cm, task.target_y_cm, robot.x_cm, robot.y_cm)
    {
        let dx = (tx - rx).abs() as f64;
        let dy = (ty - ry).abs() as f64;
        (dx * dx + dy * dy).sqrt() / 5000.0
    } else {
        0.5
    };

    // Load: normalize by active task count (cap at 10)
    let load_score = (robot.active_tasks as f64 / 10.0).min(1.0);

    // Battery: invert (higher battery = better = lower score)
    let battery_score = 1.0 - (robot.battery.unwrap_or(100) as f64 / 100.0);

    // Affinity: 0 if preferred robot matches, 0.5 otherwise
    let affinity_score = match task.preferred_robot_short_id {
        Some(pref) if pref == robot.short_id => 0.0,
        Some(_) => 1.0,
        None => 0.5, // no preference
    };

    WEIGHT_DISTANCE * distance_score
        + WEIGHT_LOAD * load_score
        + WEIGHT_BATTERY * battery_score
        + WEIGHT_AFFINITY * affinity_score
}

/// Select the best robot for a task from a list of candidates.
/// Returns the robot info if a suitable candidate exists.
pub fn select_best_robot<'a>(
    task: &Task,
    robots: &'a [RobotInfo],
) -> Option<&'a RobotInfo> {
    let task_type = TaskType::from_str(&task.task_type)?;
    let required_caps = task_type.required_capabilities();

    robots
        .iter()
        .filter(|r| r.status == 1 && r.battery.unwrap_or(0) > 20) // online + battery
        .filter(|r| {
            // Check capabilities
            required_caps.iter().all(|cap| {
                r.capabilities
                    .get(cap)
                    .and_then(|v| v.as_bool())
                    .unwrap_or(false)
            })
        })
        .min_by(|a, b| {
            let sa = score_robot(a, task);
            let sb = score_robot(b, task);
            sa.partial_cmp(&sb).unwrap_or(std::cmp::Ordering::Equal)
        })
}

// ── Robot info query ───────────────────────────────────────────

/// Fetch robot info from the database (with position join).
pub async fn get_available_robots(pool: &PgPool) -> Result<Vec<RobotInfo>, Error> {
    #[derive(sqlx::FromRow)]
    struct RobotRow {
        id: i64,
        short_id: i16,
        machine_id: Vec<u8>,
        name: String,
        capabilities: serde_json::Value,
        status: i16,
        battery: Option<i16>,
        x_cm: Option<i32>,
        y_cm: Option<i32>,
        heading: Option<i16>,
        active_tasks: Option<i64>,
    }

    let rows: Vec<RobotRow> = sqlx::query_as::<_, RobotRow>(
        r#"SELECT
             r.id, r.short_id, r.machine_id, r.name,
             r.capabilities, r.status, r.battery,
             p.x_cm, p.y_cm, p.heading,
             COALESCE(t.active_count, 0) as active_tasks
           FROM robots r
           LEFT JOIN robot_positions p ON r.id = p.robot_id
           LEFT JOIN (
             SELECT target_robot_short_id, COUNT(*) as active_count
             FROM tasks WHERE status = 1
             GROUP BY target_robot_short_id
           ) t ON r.short_id = t.target_robot_short_id
           WHERE r.status = 1"#
    )
    .fetch_all(pool)
    .await?;

    Ok(rows
        .into_iter()
        .map(|r| RobotInfo {
            id: r.id,
            short_id: r.short_id,
            machine_id_hex: hex::encode(&r.machine_id),
            name: r.name,
            capabilities: r.capabilities,
            status: r.status,
            battery: r.battery,
            x_cm: r.x_cm,
            y_cm: r.y_cm,
            heading: r.heading,
            active_tasks: r.active_tasks.unwrap_or(0) as i32,
        })
        .collect())
}

// ── Task dispatch loop ────────────────────────────────────────

/// Background task that periodically checks for pending tasks
/// and attempts to assign them to available robots.
pub async fn run_dispatch_loop(
    _config: Config,
    pool: PgPool,
    nats: async_nats::Client,
) -> ! {
    use std::time::Duration;

    let interval = Duration::from_secs(2);
    tracing::info!("task dispatch loop started (interval=2s)");

    loop {
        tokio::time::sleep(interval).await;

        // Get pending tasks (ordered by priority)
        let pending = match list_pending_tasks(&pool).await {
            Ok(tasks) => tasks,
            Err(e) => {
                tracing::error!("failed to fetch pending tasks: {}", e);
                continue;
            }
        };

        if pending.is_empty() {
            continue;
        }

        // Get available robots
        let robots = match get_available_robots(&pool).await {
            Ok(r) => r,
            Err(e) => {
                tracing::error!("failed to fetch available robots: {}", e);
                continue;
            }
        };

        if robots.is_empty() {
            tracing::debug!("no robots available for {} pending tasks", pending.len());
            continue;
        }

        for task in &pending {
            // If task specifies a target robot, use it directly
            if let Some(short_id) = task.target_robot_short_id {
                if let Some(robot) = robots.iter().find(|r| r.short_id == short_id) {
                    dispatch_task(&pool, &nats, task, robot).await;
                }
                continue;
            }

            // Run assignment algorithm
            if let Some(robot) = select_best_robot(task, &robots) {
                dispatch_task(&pool, &nats, task, robot).await;
            } else {
                tracing::debug!(
                    "no suitable robot for task id={} type={}",
                    task.id,
                    task.task_type
                );
            }
        }
    }
}

async fn dispatch_task(
    pool: &PgPool,
    nats: &async_nats::Client,
    task: &Task,
    robot: &RobotInfo,
) {
    // Update task: assign robot, set Running
    if let Err(e) = assign_robot(pool, task.id, robot.short_id).await {
        tracing::error!("failed to assign robot to task {}: {}", task.id, e);
        return;
    }

    match update_task_status(pool, task.id, TaskStatus::Running).await {
        Ok(_) => {
            metrics::TASKS_ASSIGNED.inc();
            tracing::info!(
                "task id={} type={} assigned to robot short_id={}",
                task.id,
                task.task_type,
                robot.short_id
            );
        }
        Err(e) => {
            tracing::error!("failed to update task status: {}", e);
            return;
        }
    }

    // Publish task to NATS for gateway → robot delivery
    let subject = format!("robot.{}.task", robot.short_id);
    let payload = serde_json::json!({
        "task_id": task.id,
        "task_type": task.task_type,
        "priority": task.priority,
        "params": task.params,
        "target_x_cm": task.target_x_cm,
        "target_y_cm": task.target_y_cm,
        "timeout_secs": task.timeout_secs,
    });

    if let Err(e) = nats
        .publish(subject, serde_json::to_vec(&payload).unwrap().into())
        .await
    {
        tracing::error!("failed to dispatch task to NATS: {}", e);
        metrics::TASKS_FAILED.inc();
    } else {
        metrics::TASKS_DISPATCHED.inc();
    }
}

/// Handle task status updates from NATS (robot → scheduler).
pub async fn set_task_completed(pool: &PgPool, id: i64) -> Result<(), Error> {
    update_task_status(pool, id, TaskStatus::Completed).await?;
    metrics::TASKS_COMPLETED.inc();
    Ok(())
}

pub async fn set_task_failed(pool: &PgPool, id: i64) -> Result<(), Error> {
    let task = get_task(pool, id).await?.ok_or_else(|| Error::NotFound(format!("task id={}", id)))?;

    if task.can_retry() {
        increment_retry(pool, id).await?;
        tracing::info!("task id={} retry {}/{}", id, task.retry_count + 1, task.max_retries);
    } else {
        update_task_status(pool, id, TaskStatus::Failed).await?;
        metrics::TASKS_FAILED.inc();
        tracing::warn!("task id={} failed after {} retries", id, task.max_retries);
    }

    Ok(())
}

// ── Tests ───────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use crate::task::{RobotInfo, Task};
    use chrono::Utc;

    fn make_robot(short_id: i16, status: i16, battery: i16, active_tasks: i32,
                  x: i32, y: i32, caps: serde_json::Value) -> RobotInfo {
        RobotInfo {
            id: short_id as i64,
            short_id,
            machine_id_hex: format!("robot{:02x}", short_id),
            name: format!("Robot{}", short_id),
            capabilities: caps,
            status,
            battery: Some(battery),
            x_cm: Some(x),
            y_cm: Some(y),
            heading: Some(0),
            active_tasks,
        }
    }

    fn make_task(task_type: &str, preferred: Option<i16>, tx: Option<i32>, ty: Option<i32>) -> Task {
        Task {
            id: 1,
            task_type: task_type.to_string(),
            priority: 128,
            target_robot_short_id: None,
            preferred_robot_short_id: preferred,
            status: 0,
            params: serde_json::json!({}),
            target_x_cm: tx,
            target_y_cm: ty,
            created_at: Utc::now(),
            started_at: None,
            completed_at: None,
            timeout_secs: 30,
            retry_count: 0,
            max_retries: 3,
        }
    }

    fn movement_caps() -> serde_json::Value {
        serde_json::json!({"movement": true})
    }

    fn dialogue_caps() -> serde_json::Value {
        serde_json::json!({"movement": true, "audio": true, "expression": true})
    }

    // ── score_robot tests ────────────────────────────────────

    #[test]
    fn test_score_robot_perfect_match() {
        let robot = make_robot(1, 1, 100, 0, 100, 100, movement_caps());
        let task = make_task("patrol", Some(1), Some(100), Some(100));

        let score = score_robot(&robot, &task);
        // Perfect match: distance=0, load=0, battery=0, affinity=0 → score=0
        assert!(score < 0.01, "perfect match should score near 0, got {}", score);
    }

    #[test]
    fn test_score_robot_low_battery_penalty() {
        let robot_low = make_robot(1, 1, 10, 0, 0, 0, movement_caps());
        let robot_high = make_robot(2, 1, 90, 0, 0, 0, movement_caps());
        let task = make_task("patrol", None, None, None);

        let score_low = score_robot(&robot_low, &task);
        let score_high = score_robot(&robot_high, &task);

        assert!(score_low > score_high,
            "low battery robot (score={}) should score worse than high battery (score={})",
            score_low, score_high);
    }

    #[test]
    fn test_score_robot_high_load_penalty() {
        let robot_busy = make_robot(1, 1, 80, 8, 0, 0, movement_caps());
        let robot_idle = make_robot(2, 1, 80, 0, 0, 0, movement_caps());
        let task = make_task("patrol", None, None, None);

        let score_busy = score_robot(&robot_busy, &task);
        let score_idle = score_robot(&robot_idle, &task);

        assert!(score_busy > score_idle,
            "busy robot (score={}) should score worse than idle (score={})",
            score_busy, score_idle);
    }

    #[test]
    fn test_score_robot_distance_matters() {
        let robot_near = make_robot(1, 1, 80, 0, 50, 50, movement_caps());
        let robot_far = make_robot(2, 1, 80, 0, 4000, 4000, movement_caps());
        let task = make_task("patrol", None, Some(0), Some(0));

        let score_near = score_robot(&robot_near, &task);
        let score_far = score_robot(&robot_far, &task);

        assert!(score_near < score_far,
            "near robot (score={}) should score better than far (score={})",
            score_near, score_far);
    }

    #[test]
    fn test_score_robot_preferred_affinity() {
        let robot_pref = make_robot(1, 1, 80, 0, 0, 0, movement_caps());
        let robot_other = make_robot(2, 1, 80, 0, 0, 0, movement_caps());
        let task = make_task("patrol", Some(1), None, None);

        let score_pref = score_robot(&robot_pref, &task);
        let score_other = score_robot(&robot_other, &task);

        assert!(score_pref < score_other,
            "preferred robot (score={}) should score better than other (score={})",
            score_pref, score_other);
    }

    // ── select_best_robot tests ──────────────────────────────

    #[test]
    fn test_select_best_robot_returns_lowest_score() {
        let robot_a = make_robot(1, 1, 90, 1, 0, 0, movement_caps());
        let robot_b = make_robot(2, 1, 100, 0, 0, 0, movement_caps());
        let robots = vec![robot_a, robot_b];
        let task = make_task("patrol", None, None, None);

        let selected = select_best_robot(&task, &robots);
        assert!(selected.is_some());
        // robot_b should be selected: better battery + no load
        assert_eq!(selected.unwrap().short_id, 2);
    }

    #[test]
    fn test_select_best_robot_filters_offline() {
        let robot = make_robot(1, 0, 100, 0, 0, 0, movement_caps());
        let robots = vec![robot];
        let task = make_task("patrol", None, None, None);

        assert!(select_best_robot(&task, &robots).is_none());
    }

    #[test]
    fn test_select_best_robot_filters_low_battery() {
        let robot = make_robot(1, 1, 15, 0, 0, 0, movement_caps());
        let robots = vec![robot];
        let task = make_task("patrol", None, None, None);

        assert!(select_best_robot(&task, &robots).is_none());
    }

    #[test]
    fn test_select_best_robot_capability_filter() {
        let robot = make_robot(1, 1, 80, 0, 0, 0, movement_caps());
        let robots = vec![robot];
        // Dialogue task requires audio + expression in addition to movement
        let task = make_task("dialogue", None, None, None);

        assert!(select_best_robot(&task, &robots).is_none(),
            "robot without audio/expression caps should not be selected for dialogue");
    }

    #[test]
    fn test_select_best_robot_capability_match() {
        let robot = make_robot(1, 1, 80, 0, 0, 0, dialogue_caps());
        let robots = vec![robot];
        let task = make_task("dialogue", None, None, None);

        let selected = select_best_robot(&task, &robots);
        assert!(selected.is_some(), "robot with full dialogue caps should be selected");
    }

    #[test]
    fn test_select_best_robot_no_candidates() {
        let robots: Vec<RobotInfo> = vec![];
        let task = make_task("patrol", None, None, None);

        assert!(select_best_robot(&task, &robots).is_none());
    }

    // ── Task state machine tests ─────────────────────────────

    #[test]
    fn test_task_can_retry() {
        let task = Task { retry_count: 2, max_retries: 3, ..make_task("patrol", None, None, None) };
        assert!(task.can_retry());
    }

    #[test]
    fn test_task_cannot_retry_when_exhausted() {
        let task = Task { retry_count: 3, max_retries: 3, ..make_task("patrol", None, None, None) };
        assert!(!task.can_retry());
    }

    #[test]
    fn test_task_terminal_states() {
        let mut task = make_task("patrol", None, None, None);
        task.status = 2; // Completed
        assert!(task.is_terminal());
        task.status = 3; // Failed
        assert!(task.is_terminal());
        task.status = 4; // Cancelled
        assert!(task.is_terminal());
        task.status = 0; // Pending
        assert!(!task.is_terminal());
        task.status = 1; // Running
        assert!(!task.is_terminal());
    }

    #[test]
    fn test_task_type_from_str_valid() {
        assert!(TaskType::from_str("patrol").is_some());
        assert!(TaskType::from_str("dialogue").is_some());
        assert!(TaskType::from_str("transport").is_some());
        assert!(TaskType::from_str("charge").is_some());
        assert!(TaskType::from_str("formation").is_some());
        assert!(TaskType::from_str("custom").is_some());
    }

    #[test]
    fn test_task_type_from_str_invalid() {
        assert!(TaskType::from_str("nonexistent").is_none());
        assert!(TaskType::from_str("").is_none());
    }

    #[test]
    fn test_required_capabilities() {
        assert_eq!(TaskType::Patrol.required_capabilities(), &["movement"]);
        assert_eq!(TaskType::Dialogue.required_capabilities(), &["movement", "audio", "expression"]);
        assert_eq!(TaskType::Transport.required_capabilities(), &["movement", "charging"]);
        assert_eq!(TaskType::Charge.required_capabilities(), &["movement", "charging"]);
        assert_eq!(TaskType::Formation.required_capabilities(), &["movement", "swarm"]);
        assert_eq!(TaskType::Custom.required_capabilities(), &["movement"]);
    }
}
