/// REST API handlers for the Scheduler.
///
/// Endpoints:
///   GET    /health                  — health check
///   POST   /api/v1/tasks            — create new task
///   GET    /api/v1/tasks            — list all tasks
///   GET    /api/v1/tasks/{id}       — get task by ID
///   DELETE /api/v1/tasks/{id}       — cancel task
///   GET    /api/v1/robots/available — list available robots
///   GET    /api/v1/stats            — scheduling statistics

use axum::{
    extract::{Path, State},
    routing::get,
    Json, Router,
};
use sqlx::postgres::PgPool;

use crate::error::Error;
use crate::scheduler;
use crate::task::{CreateTaskRequest, Task, TaskListResponse, TaskStatus};

#[derive(Clone)]
pub struct AppState {
    pub pool: PgPool,
    pub nats: async_nats::Client,
}

pub fn router(state: AppState) -> Router {
    Router::new()
        .route("/health", get(health))
        .route("/api/v1/tasks", get(list_tasks).post(create_task))
        .route("/api/v1/tasks/{id}", get(get_task).delete(cancel_task))
        .route("/api/v1/robots/available", get(available_robots))
        .route("/api/v1/stats", get(stats))
        .with_state(state)
}

async fn health() -> &'static str {
    "OK"
}

async fn create_task(
    State(state): State<AppState>,
    Json(req): Json<CreateTaskRequest>,
) -> Result<(axum::http::StatusCode, Json<Task>), Error> {
    let task = scheduler::create_task(&state.pool, &req).await?;
    Ok((axum::http::StatusCode::CREATED, Json(task)))
}

async fn list_tasks(State(state): State<AppState>) -> Result<Json<TaskListResponse>, Error> {
    let tasks = scheduler::list_tasks(&state.pool).await?;
    let count = tasks.len();
    Ok(Json(TaskListResponse { tasks, count }))
}

async fn get_task(
    State(state): State<AppState>,
    Path(id): Path<i64>,
) -> Result<Json<Task>, Error> {
    let task = scheduler::get_task(&state.pool, id)
        .await?
        .ok_or_else(|| Error::NotFound(format!("task id={}", id)))?;
    Ok(Json(task))
}

async fn cancel_task(
    State(state): State<AppState>,
    Path(id): Path<i64>,
) -> Result<axum::http::StatusCode, Error> {
    let task = scheduler::get_task(&state.pool, id)
        .await?
        .ok_or_else(|| Error::NotFound(format!("task id={}", id)))?;

    if task.is_terminal() {
        return Err(Error::InvalidInput("task already in terminal state".into()));
    }

    let _ = scheduler::update_task_status(&state.pool, id, TaskStatus::Cancelled).await?;

    // Notify robot to cancel via NATS
    if let Some(short_id) = task.target_robot_short_id {
        let subject = format!("robot.{}.task", short_id);
        let payload = serde_json::json!({
            "task_id": id,
            "action": "cancel",
        });
        let _ = state.nats.publish(subject, serde_json::to_vec(&payload).unwrap().into()).await;
    }

    Ok(axum::http::StatusCode::NO_CONTENT)
}

async fn available_robots(
    State(state): State<AppState>,
) -> Result<Json<serde_json::Value>, Error> {
    let robots = scheduler::get_available_robots(&state.pool).await?;
    Ok(Json(serde_json::json!({
        "robots": robots,
        "count": robots.len(),
    })))
}

#[derive(serde::Serialize)]
struct StatsResponse {
    pending_tasks: i64,
    running_tasks: i64,
    completed_tasks: i64,
    failed_tasks: i64,
    cancelled_tasks: i64,
}

async fn stats(State(state): State<AppState>) -> Result<Json<StatsResponse>, Error> {
    #[derive(sqlx::FromRow)]
    struct CountRow {
        status: i16,
        count: i64,
    }

    let rows: Vec<CountRow> = sqlx::query_as::<_, CountRow>(
        "SELECT status, COUNT(*) as count FROM tasks GROUP BY status"
    )
    .fetch_all(&state.pool)
    .await?;

    let mut stats = StatsResponse {
        pending_tasks: 0,
        running_tasks: 0,
        completed_tasks: 0,
        failed_tasks: 0,
        cancelled_tasks: 0,
    };

    for row in &rows {
        match row.status {
            0 => stats.pending_tasks = row.count,
            1 => stats.running_tasks = row.count,
            2 => stats.completed_tasks = row.count,
            3 => stats.failed_tasks = row.count,
            4 => stats.cancelled_tasks = row.count,
            _ => {}
        }
    }

    Ok(Json(stats))
}
