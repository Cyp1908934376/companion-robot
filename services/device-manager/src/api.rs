/// REST API handlers via axum.
///
/// Endpoints:
///   GET    /health                  — health check
///   GET    /api/v1/robots           — list all robots
///   GET    /api/v1/robots/:id       — get robot by ID
///   POST   /api/v1/robots/register  — register new robot
///   PUT    /api/v1/robots/:id       — update robot
///   DELETE /api/v1/robots/:id       — delete robot
///   GET    /api/v1/robots/:id/position — get latest position
///   GET    /api/v1/stats            — cluster statistics

use axum::{
    extract::{Path, State},
    routing::{get, post},
    Json, Router,
};
use serde::Serialize;
use sqlx::postgres::PgPool;

use crate::db;
use crate::error::Error;

#[derive(Clone)]
pub struct AppState {
    pub pool: PgPool,
    #[allow(dead_code)]
    pub redis: redis::aio::ConnectionManager,
}

impl AppState {
    pub fn new(pool: PgPool, redis: redis::aio::ConnectionManager) -> Self {
        Self { pool, redis }
    }
}

pub fn router(state: AppState) -> Router {
    Router::new()
        .route("/health", get(health))
        .route("/api/v1/robots", get(list_robots))
        .route("/api/v1/robots/{id}", get(get_robot).put(update_robot).delete(delete_robot))
        .route("/api/v1/robots/register", post(register_robot))
        .route("/api/v1/robots/{id}/position", get(get_position))
        .route("/api/v1/stats", get(stats))
        .with_state(state)
}

// ── Health ─────────────────────────────────────────────────────

async fn health() -> &'static str {
    "OK"
}

// ── Robot CRUD ─────────────────────────────────────────────────

#[derive(Serialize)]
struct RobotListResponse {
    robots: Vec<db::Robot>,
    count: usize,
}

async fn list_robots(State(state): State<AppState>) -> Result<Json<RobotListResponse>, Error> {
    let robots = db::list_robots(&state.pool).await?;
    let count = robots.len();
    Ok(Json(RobotListResponse { robots, count }))
}

async fn get_robot(
    State(state): State<AppState>,
    Path(id): Path<i64>,
) -> Result<Json<db::Robot>, Error> {
    let robot = db::get_robot_by_id(&state.pool, id)
        .await?
        .ok_or_else(|| Error::NotFound(format!("robot id={}", id)))?;

    Ok(Json(robot))
}

async fn register_robot(
    State(state): State<AppState>,
    Json(req): Json<db::RegisterRobotRequest>,
) -> Result<(axum::http::StatusCode, Json<db::Robot>), Error> {
    let robot = db::register_robot(&state.pool, &req).await?;
    Ok((axum::http::StatusCode::CREATED, Json(robot)))
}

async fn update_robot(
    State(state): State<AppState>,
    Path(id): Path<i64>,
    Json(req): Json<db::UpdateRobotRequest>,
) -> Result<Json<db::Robot>, Error> {
    let robot = db::update_robot(&state.pool, id, &req).await?;
    Ok(Json(robot))
}

async fn delete_robot(
    State(state): State<AppState>,
    Path(id): Path<i64>,
) -> Result<axum::http::StatusCode, Error> {
    db::delete_robot(&state.pool, id).await?;
    Ok(axum::http::StatusCode::NO_CONTENT)
}

// ── Position ───────────────────────────────────────────────────

async fn get_position(
    State(state): State<AppState>,
    Path(id): Path<i64>,
) -> Result<Json<db::RobotPosition>, Error> {
    // Read from robot_positions table via a query
    let pos = sqlx::query_as::<_, db::RobotPosition>(
        "SELECT * FROM robot_positions WHERE robot_id = $1"
    )
    .bind(id)
    .fetch_optional(&state.pool)
    .await?
    .ok_or_else(|| Error::NotFound(format!("position for robot id={}", id)))?;

    Ok(Json(pos))
}

// ── Stats ──────────────────────────────────────────────────────

#[derive(Serialize)]
struct StatsResponse {
    total_robots: i64,
    online_robots: i64,
    offline_robots: i64,
    busy_robots: i64,
    error_robots: i64,
}

async fn stats(State(state): State<AppState>) -> Result<Json<StatsResponse>, Error> {
    #[derive(sqlx::FromRow)]
    struct CountRow {
        status: i16,
        count: i64,
    }

    let rows: Vec<CountRow> = sqlx::query_as::<_, CountRow>(
        "SELECT status, COUNT(*) as count FROM robots GROUP BY status"
    )
    .fetch_all(&state.pool)
    .await?;

    let mut stats = StatsResponse {
        total_robots: 0,
        online_robots: 0,
        offline_robots: 0,
        busy_robots: 0,
        error_robots: 0,
    };

    for row in &rows {
        stats.total_robots += row.count;
        match row.status {
            0 => stats.offline_robots += row.count,
            1 => stats.online_robots += row.count,
            2 => stats.busy_robots += row.count,
            3 => stats.error_robots += row.count,
            _ => {}
        }
    }

    Ok(Json(stats))
}
