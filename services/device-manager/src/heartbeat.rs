/// Heartbeat monitor — detects offline robots.
///
/// Periodically checks Redis for stale heartbeats.
/// When a robot's heartbeat expires (TTL-based), it's marked offline
/// in both Redis and PostgreSQL, and an event is published to NATS.

use chrono::{DateTime, Utc};
use redis::aio::ConnectionManager;
use sqlx::postgres::PgPool;
use std::sync::Arc;
use std::time::Duration;

use crate::config::Config;
use crate::db;
use crate::metrics;
use crate::redis as redis_ops;

/// Check whether a heartbeat timestamp has exceeded the timeout.
/// Extracted as a pure function for testability.
pub fn is_heartbeat_expired(
    last_heartbeat_rfc3339: &str,
    now: DateTime<Utc>,
    timeout_secs: i64,
) -> bool {
    let timeout_dur = chrono::Duration::seconds(timeout_secs);
    match DateTime::parse_from_rfc3339(last_heartbeat_rfc3339) {
        Ok(last_hb) => {
            let last_hb: DateTime<Utc> = last_hb.into();
            now - last_hb > timeout_dur
        }
        Err(_) => false, // unparseable timestamp → not expired (graceful)
    }
}

/// Run the heartbeat monitor loop.
/// Exits only on fatal error or when all connections are lost.
pub async fn run(
    config: Config,
    pool: PgPool,
    redis: ConnectionManager,
    nats_client: async_nats::Client,
) -> ! {
    let redis = Arc::new(tokio::sync::Mutex::new(redis));
    let check_interval = Duration::from_secs(config.heartbeat_check_interval_secs);

    tracing::info!(
        "heartbeat monitor started (timeout={}s interval={}s)",
        config.heartbeat_timeout_secs,
        config.heartbeat_check_interval_secs
    );

    loop {
        tokio::time::sleep(check_interval).await;

        let mut redis_guard = redis.lock().await;

        // Get the set of machine_ids currently in the online set
        let online_set = match redis_ops::get_online_set(&mut redis_guard).await {
            Ok(set) => set,
            Err(e) => {
                tracing::error!("failed to read Redis online set: {}", e);
                continue;
            }
        };

        let now = Utc::now();

        for hex_id in &online_set {
            let machine_id: Vec<u8> = match hex::decode(hex_id) {
                Ok(id) => id,
                Err(_) => continue,
            };

            // Check cached status
            let cached = match redis_ops::get_cached_status(&mut redis_guard, &machine_id).await {
                Ok(Some(s)) => s,
                Ok(None) => {
                    // TTL expired — mark offline
                    mark_offline_inner(
                        &pool,
                        &mut redis_guard,
                        &machine_id,
                        &nats_client,
                    ).await;
                    continue;
                }
                Err(e) => {
                    tracing::warn!("Redis error reading cached status: {}", e);
                    continue;
                }
            };

            // Parse heartbeat timestamp
            if is_heartbeat_expired(&cached.last_heartbeat, now, config.heartbeat_timeout_secs as i64) {
                tracing::warn!(
                    "robot heartbeat timeout: {} (last: {})",
                    hex_id,
                    cached.last_heartbeat
                );
                mark_offline_inner(
                    &pool,
                    &mut redis_guard,
                    &machine_id,
                    &nats_client,
                ).await;
            }
        }

        // Update prometheus metric
        metrics::ONLINE_ROBOTS.set(online_set.len() as i64);
    }
}

async fn mark_offline_inner(
    pool: &PgPool,
    redis: &mut ConnectionManager,
    machine_id: &[u8],
    nats: &async_nats::Client,
) {
    // Remove from Redis
    if let Err(e) = redis_ops::set_offline(redis, machine_id).await {
        tracing::error!("failed to mark offline in Redis: {}", e);
    }

    // Update database
    if let Err(e) = db::mark_offline(pool, machine_id).await {
        tracing::error!("failed to mark offline in DB: {}", e);
    } else {
        metrics::ROBOT_OFFLINE_EVENTS.inc();
    }

    // Publish status change to NATS
    let subject = format!("internal.device.status");
    let payload = serde_json::json!({
        "machine_id": hex::encode(machine_id),
        "status": "offline",
        "reason": "heartbeat_timeout",
    });
    if let Err(e) = nats.publish(subject, serde_json::to_vec(&payload).unwrap().into()).await {
        tracing::error!("failed to publish offline event to NATS: {}", e);
    }
}

// ── Tests ───────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use chrono::{Utc, TimeZone};

    #[test]
    fn test_heartbeat_not_expired_recent() {
        let now = Utc::now();
        let recent = now - chrono::Duration::seconds(3);
        let timestamp = recent.to_rfc3339();

        assert!(!is_heartbeat_expired(&timestamp, now, 10));
    }

    #[test]
    fn test_heartbeat_not_expired_boundary() {
        let now = Utc.with_ymd_and_hms(2026, 6, 9, 12, 0, 0).unwrap();
        let just_within = now - chrono::Duration::seconds(15);
        let timestamp = just_within.to_rfc3339();

        assert!(!is_heartbeat_expired(&timestamp, now, 15));
    }

    #[test]
    fn test_heartbeat_expired() {
        let now = Utc.with_ymd_and_hms(2026, 6, 9, 12, 0, 0).unwrap();
        let stale = now - chrono::Duration::seconds(31);
        let timestamp = stale.to_rfc3339();

        assert!(is_heartbeat_expired(&timestamp, now, 30));
    }

    #[test]
    fn test_heartbeat_expired_long_timeout() {
        let now = Utc.with_ymd_and_hms(2026, 6, 9, 12, 0, 0).unwrap();
        let stale = now - chrono::Duration::seconds(61);
        let timestamp = stale.to_rfc3339();

        assert!(is_heartbeat_expired(&timestamp, now, 60));
    }

    #[test]
    fn test_heartbeat_invalid_timestamp_graceful() {
        // Invalid timestamps should not trigger expiration (fail-safe)
        assert!(!is_heartbeat_expired("not-a-timestamp", Utc::now(), 10));
        assert!(!is_heartbeat_expired("", Utc::now(), 10));
    }
}
