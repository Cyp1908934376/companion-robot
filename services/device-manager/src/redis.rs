/// Redis state cache — fast online status and metrics.
///
/// Keys:
///   robot:{machine_id_hex}:status     — JSON { status, battery, rssi, last_heartbeat }
///   robot:{machine_id_hex}:position   — JSON { x_cm, y_cm, heading }
///   robot:online:set                  — SET of online machine_id_hex values

use redis::aio::ConnectionManager;
use redis::AsyncCommands;
use serde::{Deserialize, Serialize};

use crate::error::Error;

// ── Redis key helpers ──────────────────────────────────────────

fn machine_hex(machine_id: &[u8]) -> String {
    hex::encode(machine_id)
}

fn status_key(machine_hex: &str) -> String {
    format!("robot:{}:status", machine_hex)
}

#[allow(dead_code)]
fn position_key(machine_hex: &str) -> String {
    format!("robot:{}:position", machine_hex)
}

const ONLINE_SET: &str = "robot:online:set";

// ── Cached types ───────────────────────────────────────────────

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CachedStatus {
    pub status: i16,
    pub battery: Option<i16>,
    pub rssi: Option<i16>,
    pub last_heartbeat: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[allow(dead_code)]
pub struct CachedPosition {
    pub x_cm: i32,
    pub y_cm: i32,
    pub heading: i16,
}

// ── Redis operations ───────────────────────────────────────────

pub async fn connect(url: &str) -> Result<ConnectionManager, Error> {
    let client = redis::Client::open(url)?;
    let conn = ConnectionManager::new(client).await?;
    tracing::info!("connected to Redis");
    Ok(conn)
}

pub async fn set_online(
    redis: &mut ConnectionManager,
    machine_id: &[u8],
    status: &CachedStatus,
) -> Result<(), Error> {
    let hex = machine_hex(machine_id);
    let json = serde_json::to_string(status)?;

    let _: () = redis.set(status_key(&hex), &json).await?;
    let _: () = redis.sadd(ONLINE_SET, &hex).await?;

    // Set TTL slightly longer than heartbeat timeout for automatic cleanup
    let _: () = redis.expire(status_key(&hex), 30).await?;

    Ok(())
}

pub async fn set_offline(
    redis: &mut ConnectionManager,
    machine_id: &[u8],
) -> Result<(), Error> {
    let hex = machine_hex(machine_id);
    let _: () = redis.srem(ONLINE_SET, &hex).await?;
    let _: () = redis.del(status_key(&hex)).await?;
    Ok(())
}

pub async fn get_online_set(
    redis: &mut ConnectionManager,
) -> Result<Vec<String>, Error> {
    let members: Vec<String> = redis.smembers(ONLINE_SET).await?;
    Ok(members)
}

pub async fn get_cached_status(
    redis: &mut ConnectionManager,
    machine_id: &[u8],
) -> Result<Option<CachedStatus>, Error> {
    let hex = machine_hex(machine_id);
    let json: Option<String> = redis.get(status_key(&hex)).await?;
    match json {
        Some(s) => Ok(Some(serde_json::from_str(&s)?)),
        _ => Ok(None),
    }
}

#[allow(dead_code)]
pub async fn update_position_cache(
    redis: &mut ConnectionManager,
    machine_id: &[u8],
    pos: &CachedPosition,
) -> Result<(), Error> {
    let hex = machine_hex(machine_id);
    let json = serde_json::to_string(pos)?;
    let _: () = redis.set(position_key(&hex), &json).await?;
    let _: () = redis.expire(position_key(&hex), 60).await?;

    Ok(())
}

#[allow(dead_code)]
pub async fn get_cached_position(
    redis: &mut ConnectionManager,
    machine_id: &[u8],
) -> Result<Option<CachedPosition>, Error> {
    let hex = machine_hex(machine_id);
    let json: Option<String> = redis.get(position_key(&hex)).await?;
    match json {
        Some(s) => Ok(Some(serde_json::from_str(&s)?)),
        _ => Ok(None),
    }
}
