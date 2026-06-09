/// NATS event subscriber for real-time robot status updates.
///
/// Subscribes to:
///   robot.*.heartbeat   → update Redis + PostgreSQL heartbeat
///   robot.*.sensor      → insert sensor data into TimescaleDB
///   robot.*.event       → forward relevant events

use async_nats::Client;
use chrono::Utc;
use futures_util::StreamExt;
use redis::aio::ConnectionManager;
use sqlx::postgres::PgPool;
use std::sync::Arc;

use crate::db;
use crate::metrics;
use crate::redis as redis_ops;
use crate::redis::CachedStatus;

/// Subscribe to NATS subjects for robot events.
/// This function spawns internal tasks for each subscription.
pub async fn subscribe_all(
    nats: Client,
    pool: PgPool,
    redis: ConnectionManager,
) -> Result<(), crate::error::Error> {
    let redis = Arc::new(tokio::sync::Mutex::new(redis));

    // Subscribe to heartbeat events
    {
        let nats = nats.clone();
        let pool = pool.clone();
        let redis = redis.clone();

        tokio::spawn(async move {
            if let Err(e) = subscribe_heartbeats(nats, pool, redis).await {
                tracing::error!("heartbeat subscriber exited: {}", e);
            }
        });
    }

    // Subscribe to sensor data
    {
        let nats = nats.clone();
        let pool = pool.clone();

        tokio::spawn(async move {
            if let Err(e) = subscribe_sensors(nats, pool).await {
                tracing::error!("sensor subscriber exited: {}", e);
            }
        });
    }

    Ok(())
}

async fn subscribe_heartbeats(
    nats: Client,
    pool: PgPool,
    redis: Arc<tokio::sync::Mutex<ConnectionManager>>,
) -> Result<(), crate::error::Error> {
    let mut subscriber = nats
        .subscribe("robot.*.heartbeat")
        .await
        .map_err(|e| crate::error::Error::Nats(e.to_string()))?;

    tracing::info!("subscribed to robot.*.heartbeat");

    while let Some(msg) = subscriber.next().await {
        // Parse message
        let payload = match serde_json::from_slice::<serde_json::Value>(&msg.payload) {
            Ok(v) => v,
            Err(e) => {
                tracing::warn!("invalid heartbeat payload: {}", e);
                continue;
            }
        };

        // Extract fields
        let machine_id_hex = payload
            .get("machine_id")
            .and_then(|v| v.as_str())
            .unwrap_or("");

        let machine_id: Vec<u8> = match hex::decode(machine_id_hex) {
            Ok(id) => id,
            Err(_) => {
                tracing::warn!("invalid machine_id in heartbeat: {}", machine_id_hex);
                continue;
            }
        };

        let battery = payload.get("battery").and_then(|v| v.as_i64()).map(|v| v as i16);
        let rssi = payload.get("rssi").and_then(|v| v.as_i64()).map(|v| v as i16);
        let status = payload.get("status").and_then(|v| v.as_i64()).unwrap_or(1) as i16;

        // Update Redis cache
        let mut redis_guard = redis.lock().await;
        let cached = CachedStatus {
            status,
            battery,
            rssi,
            last_heartbeat: Utc::now().to_rfc3339(),
        };

        if let Err(e) = redis_ops::set_online(&mut redis_guard, &machine_id, &cached).await {
            tracing::error!("failed to update Redis heartbeat: {}", e);
        }
        drop(redis_guard);

        // Update PostgreSQL
        if let Err(e) = db::update_heartbeat(&pool, &machine_id, battery, rssi).await {
            tracing::error!("failed to update DB heartbeat: {}", e);
        }

        metrics::HEARTBEAT_COUNT.inc();
    }

    Ok(())
}

async fn subscribe_sensors(
    nats: Client,
    pool: PgPool,
) -> Result<(), crate::error::Error> {
    let mut subscriber = nats
        .subscribe("robot.*.sensor")
        .await
        .map_err(|e| crate::error::Error::Nats(e.to_string()))?;

    tracing::info!("subscribed to robot.*.sensor");

    while let Some(msg) = subscriber.next().await {
        let payload = match serde_json::from_slice::<serde_json::Value>(&msg.payload) {
            Ok(v) => v,
            Err(e) => {
                tracing::warn!("invalid sensor payload: {}", e);
                continue;
            }
        };

        // Extract robot_id from subject: robot.{short_id}.sensor
        let subject = msg.subject.to_string();
        let parts: Vec<&str> = subject.split('.').collect();
        if parts.len() < 2 {
            continue;
        }
        let short_id: i16 = match parts[1].parse() {
            Ok(id) => id,
            Err(_) => continue,
        };

        let sensor_type = payload.get("sensor_type").and_then(|v| v.as_i64()).unwrap_or(0) as i16;

        // Resolve short_id to internal robot id
        if let Ok(Some(robot)) = db::get_robot_by_short_id(&pool, short_id).await {
            if let Err(e) = db::insert_sensor_data(&pool, robot.id, sensor_type, payload.clone()).await {
                tracing::error!("failed to insert sensor data: {}", e);
            }
            metrics::SENSOR_DATA_COUNT.inc();
        }
    }

    Ok(())
}
