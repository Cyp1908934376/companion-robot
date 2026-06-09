/// NATS subscriber for task status updates from robots.
///
/// Subscribes to:
///   robot.*.task_status   — robot reports task progress
///   internal.device.status — device-manager status changes
///
/// Publishes:
///   robot.{id}.task       — task assignment to robot
///   internal.scheduler.assign — assignment event

use async_nats::Client;
use futures_util::StreamExt;
use sqlx::postgres::PgPool;

/// Subscribe to task status updates from robots.
pub async fn subscribe_task_status(
    nats: Client,
    pool: PgPool,
) -> Result<(), crate::error::Error> {
    let mut subscriber = nats
        .subscribe("robot.*.task_status")
        .await
        .map_err(|e| crate::error::Error::Nats(e.to_string()))?;

    tracing::info!("subscribed to robot.*.task_status");

    while let Some(msg) = subscriber.next().await {
        let payload = match serde_json::from_slice::<serde_json::Value>(&msg.payload) {
            Ok(v) => v,
            Err(e) => {
                tracing::warn!("invalid task_status payload: {}", e);
                continue;
            }
        };

        let task_id = match payload.get("task_id").and_then(|v| v.as_i64()) {
            Some(id) => id,
            None => {
                tracing::warn!("task_status missing task_id");
                continue;
            }
        };

        let status = payload.get("status").and_then(|v| v.as_str()).unwrap_or("");

        match status {
            "completed" | "success" => {
                if let Err(e) = crate::scheduler::set_task_completed(&pool, task_id).await {
                    tracing::error!("failed to complete task {}: {}", task_id, e);
                } else {
                    tracing::info!("task {} completed", task_id);
                }
            }
            "failed" | "error" => {
                let reason = payload.get("reason").and_then(|v| v.as_str()).unwrap_or("unknown");
                tracing::warn!("task {} failed: {}", task_id, reason);
                if let Err(e) = crate::scheduler::set_task_failed(&pool, task_id).await {
                    tracing::error!("failed to handle task failure {}: {}", task_id, e);
                }
            }
            "running" => {
                tracing::debug!("task {} running", task_id);
            }
            other => {
                tracing::debug!("task {} status: {}", task_id, other);
            }
        }
    }

    Ok(())
}

/// Subscribe to device status changes from device-manager.
pub async fn subscribe_device_status(
    nats: Client,
    pool: PgPool,
) -> Result<(), crate::error::Error> {
    let mut subscriber = nats
        .subscribe("internal.device.status")
        .await
        .map_err(|e| crate::error::Error::Nats(e.to_string()))?;

    tracing::info!("subscribed to internal.device.status");

    while let Some(msg) = subscriber.next().await {
        let payload = match serde_json::from_slice::<serde_json::Value>(&msg.payload) {
            Ok(v) => v,
            Err(e) => {
                tracing::warn!("invalid device status payload: {}", e);
                continue;
            }
        };

        let status = payload.get("status").and_then(|v| v.as_str()).unwrap_or("");
        let machine_id = payload.get("machine_id").and_then(|v| v.as_str()).unwrap_or("");

        if status == "offline" {
            tracing::warn!(
                "robot went offline: machine_id={} — cancelling pending tasks",
                machine_id
            );

            // Find tasks assigned to this robot and re-queue or fail them
            let machine_id_bytes = match hex::decode(machine_id) {
                Ok(id) => id,
                Err(_) => continue,
            };

            // Resolve to short_id
            if let Ok(Some(robot)) = sqlx::query_as::<_, (i64, i16)>(
                "SELECT id, short_id FROM robots WHERE machine_id = $1"
            )
            .bind(&machine_id_bytes)
            .fetch_optional(&pool)
            .await
            {
                // Re-queue running tasks assigned to this robot
                let _ = sqlx::query(
                    "UPDATE tasks SET status=0, target_robot_short_id=NULL WHERE target_robot_short_id=$1 AND status=1"
                )
                .bind(robot.1)
                .execute(&pool)
                .await;

                tracing::info!("re-queued tasks for offline robot short_id={}", robot.1);
            }
        }
    }

    Ok(())
}
