/// PostgreSQL models and queries.
///
/// Tables:
///   - robots:       device registry (machine_id, short_id, name, capabilities, status)
///   - robot_positions:  latest known position per robot
///   - sensor_data:  TimescaleDB hypertable for time-series sensor data

use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};
use sqlx::postgres::PgPool;
use sqlx::FromRow;

use crate::error::Error;

// ── Robot model ────────────────────────────────────────────────

#[derive(Debug, Clone, FromRow, Serialize, Deserialize)]
pub struct Robot {
    pub id: i64,
    pub machine_id: Vec<u8>,
    pub short_id: i16,
    pub name: String,
    pub capabilities: serde_json::Value,
    pub firmware_ver: String,
    pub status: i16,          // 0=offline, 1=online, 2=busy, 3=error
    pub last_heartbeat: Option<DateTime<Utc>>,
    pub battery: Option<i16>,
    pub rssi: Option<i16>,
    pub created_at: DateTime<Utc>,
    pub updated_at: DateTime<Utc>,
}

impl Robot {
    #[allow(dead_code)]
    pub fn is_online(&self) -> bool {
        self.status == 1 || self.status == 2
    }
}

#[derive(Debug, Deserialize)]
pub struct RegisterRobotRequest {
    pub machine_id: String,     // hex-encoded 128-bit
    pub short_id: i16,
    pub name: String,
    pub capabilities: serde_json::Value,
    pub firmware_ver: String,
}

#[derive(Debug, Deserialize)]
pub struct UpdateRobotRequest {
    pub name: Option<String>,
    pub capabilities: Option<serde_json::Value>,
    pub firmware_ver: Option<String>,
}

// ── Position model ─────────────────────────────────────────────

#[derive(Debug, Clone, FromRow, Serialize)]
pub struct RobotPosition {
    pub robot_id: i64,
    pub x_cm: i32,
    pub y_cm: i32,
    pub heading: i16,
    pub updated_at: DateTime<Utc>,
}

// ── Sensor data model ──────────────────────────────────────────

#[derive(Debug, Clone, FromRow, Serialize)]
#[allow(dead_code)]
pub struct SensorData {
    pub time: DateTime<Utc>,
    pub robot_id: i64,
    pub sensor_type: i16,
    pub data: serde_json::Value,
}

// ── DB queries ─────────────────────────────────────────────────

pub async fn init_db(pool: &PgPool) -> Result<(), Error> {
    sqlx::query(
        r#"
        CREATE TABLE IF NOT EXISTS robots (
            id              BIGSERIAL PRIMARY KEY,
            machine_id      BYTEA UNIQUE NOT NULL,
            short_id        SMALLINT UNIQUE NOT NULL,
            name            VARCHAR(64) NOT NULL,
            capabilities    JSONB NOT NULL DEFAULT '{}',
            firmware_ver    VARCHAR(32) NOT NULL DEFAULT '0.0.0',
            status          SMALLINT NOT NULL DEFAULT 0,
            last_heartbeat  TIMESTAMPTZ,
            battery         SMALLINT,
            rssi            SMALLINT,
            created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
            updated_at      TIMESTAMPTZ NOT NULL DEFAULT NOW()
        );

        CREATE TABLE IF NOT EXISTS robot_positions (
            robot_id    BIGINT REFERENCES robots(id) ON DELETE CASCADE,
            x_cm        INT NOT NULL DEFAULT 0,
            y_cm        INT NOT NULL DEFAULT 0,
            heading     SMALLINT NOT NULL DEFAULT 0,
            updated_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
            PRIMARY KEY (robot_id)
        );
        "#
    )
    .execute(pool)
    .await?;

    tracing::info!("database tables initialized");
    Ok(())
}

pub async fn list_robots(pool: &PgPool) -> Result<Vec<Robot>, Error> {
    Ok(sqlx::query_as::<_, Robot>("SELECT * FROM robots ORDER BY id")
        .fetch_all(pool)
        .await?)
}

pub async fn get_robot_by_id(pool: &PgPool, id: i64) -> Result<Option<Robot>, Error> {
    Ok(sqlx::query_as::<_, Robot>("SELECT * FROM robots WHERE id = $1")
        .bind(id)
        .fetch_optional(pool)
        .await?)
}

pub async fn get_robot_by_short_id(pool: &PgPool, short_id: i16) -> Result<Option<Robot>, Error> {
    Ok(sqlx::query_as::<_, Robot>("SELECT * FROM robots WHERE short_id = $1")
        .bind(short_id)
        .fetch_optional(pool)
        .await?)
}

pub async fn get_robot_by_machine_id(pool: &PgPool, machine_id: &[u8]) -> Result<Option<Robot>, Error> {
    Ok(sqlx::query_as::<_, Robot>("SELECT * FROM robots WHERE machine_id = $1")
        .bind(machine_id)
        .fetch_optional(pool)
        .await?)
}

pub async fn register_robot(pool: &PgPool, req: &RegisterRobotRequest) -> Result<Robot, Error> {
    let machine_id = hex::decode(&req.machine_id)
        .map_err(|_| Error::InvalidInput("invalid machine_id hex".into()))?;

    if let Some(_existing) = get_robot_by_machine_id(pool, &machine_id).await? {
        return Err(Error::AlreadyExists("machine_id already registered".into()));
    }

    let robot = sqlx::query_as::<_, Robot>(
        r#"INSERT INTO robots (machine_id, short_id, name, capabilities, firmware_ver)
           VALUES ($1, $2, $3, $4, $5)
           RETURNING *"#
    )
    .bind(&machine_id)
    .bind(req.short_id)
    .bind(&req.name)
    .bind(&req.capabilities)
    .bind(&req.firmware_ver)
    .fetch_one(pool)
    .await?;

    tracing::info!("robot registered: id={} short_id={}", robot.id, robot.short_id);
    Ok(robot)
}

pub async fn update_robot(pool: &PgPool, id: i64, req: &UpdateRobotRequest) -> Result<Robot, Error> {
    let existing = get_robot_by_id(pool, id).await?
        .ok_or_else(|| Error::NotFound(format!("robot id={}", id)))?;

    let name = req.name.clone().unwrap_or(existing.name);
    let capabilities = req.capabilities.clone().unwrap_or(existing.capabilities);
    let firmware_ver = req.firmware_ver.clone().unwrap_or(existing.firmware_ver);

    let robot = sqlx::query_as::<_, Robot>(
        r#"UPDATE robots SET name=$1, capabilities=$2, firmware_ver=$3, updated_at=NOW()
           WHERE id=$4 RETURNING *"#
    )
    .bind(&name)
    .bind(&capabilities)
    .bind(&firmware_ver)
    .bind(id)
    .fetch_one(pool)
    .await?;

    Ok(robot)
}

pub async fn delete_robot(pool: &PgPool, id: i64) -> Result<(), Error> {
    let result = sqlx::query("DELETE FROM robots WHERE id = $1")
        .bind(id)
        .execute(pool)
        .await?;

    if result.rows_affected() == 0 {
        return Err(Error::NotFound(format!("robot id={}", id)));
    }
    Ok(())
}

pub async fn update_heartbeat(
    pool: &PgPool,
    machine_id: &[u8],
    battery: Option<i16>,
    rssi: Option<i16>,
) -> Result<(), Error> {
    sqlx::query(
        r#"UPDATE robots
           SET status=1, last_heartbeat=NOW(), battery=$2, rssi=$3, updated_at=NOW()
           WHERE machine_id=$1"#
    )
    .bind(machine_id)
    .bind(battery)
    .bind(rssi)
    .execute(pool)
    .await?;
    Ok(())
}

pub async fn mark_offline(pool: &PgPool, machine_id: &[u8]) -> Result<(), Error> {
    sqlx::query(
        "UPDATE robots SET status=0, updated_at=NOW() WHERE machine_id=$1 AND status=1"
    )
    .bind(machine_id)
    .execute(pool)
    .await?;
    Ok(())
}

#[allow(dead_code)]
pub async fn update_position(
    pool: &PgPool,
    robot_id: i64,
    x_cm: i32,
    y_cm: i32,
    heading: i16,
) -> Result<(), Error> {
    sqlx::query(
        r#"INSERT INTO robot_positions (robot_id, x_cm, y_cm, heading, updated_at)
           VALUES ($1, $2, $3, $4, NOW())
           ON CONFLICT (robot_id) DO UPDATE SET x_cm=$2, y_cm=$3, heading=$4, updated_at=NOW()"#
    )
    .bind(robot_id)
    .bind(x_cm)
    .bind(y_cm)
    .bind(heading)
    .execute(pool)
    .await?;
    Ok(())
}

pub async fn insert_sensor_data(
    pool: &PgPool,
    robot_id: i64,
    sensor_type: i16,
    data: serde_json::Value,
) -> Result<(), Error> {
    sqlx::query(
        "INSERT INTO sensor_data (time, robot_id, sensor_type, data) VALUES (NOW(), $1, $2, $3)"
    )
    .bind(robot_id)
    .bind(sensor_type)
    .bind(&data)
    .execute(pool)
    .await?;
    Ok(())
}
