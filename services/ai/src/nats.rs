/// NATS bridge — subscribe to AI inference requests, publish results.
///
/// Subscribes to:
///   internal.ai.asr          — audio data for speech recognition
///   internal.ai.nlu           — text for NLU processing
///   internal.ai.dialogue      — dialogue turn requests
///   internal.ai.vision        — image data for vision analysis
///   internal.ai.behavior      — combined inference (ASR→NLU→dialogue→behavior)
///
/// Publishes:
///   internal.ai.result        — all inference results
///   robot.{id}.cmd            — behavior decisions → robot commands

use async_nats::Client;
use futures_util::StreamExt;
use serde_json::Value;
use std::sync::Arc;

use crate::asr;
use crate::behavior;
use crate::config::Config;
use crate::dialogue::{DialogueManager, DialogueRequest};
use crate::nlu;
use crate::vision;

/// Subscribe to all AI inference subjects.
pub async fn subscribe_all(
    config: Config,
    nats: Client,
    dialogue_manager: Arc<DialogueManager>,
) -> Result<(), crate::error::Error> {
    // ASR subscriber
    {
        let nats = nats.clone();
        let config = config.clone();
        tokio::spawn(async move {
            if let Err(e) = subscribe_asr(config, nats).await {
                tracing::error!("ASR subscriber exited: {}", e);
            }
        });
    }

    // NLU subscriber
    {
        let nats = nats.clone();
        let config = config.clone();
        tokio::spawn(async move {
            if let Err(e) = subscribe_nlu(config, nats).await {
                tracing::error!("NLU subscriber exited: {}", e);
            }
        });
    }

    // Dialogue subscriber
    {
        let nats = nats.clone();
        let config = config.clone();
        let dm = dialogue_manager.clone();
        tokio::spawn(async move {
            if let Err(e) = subscribe_dialogue(config, nats, dm).await {
                tracing::error!("Dialogue subscriber exited: {}", e);
            }
        });
    }

    // Vision subscriber
    {
        let nats = nats.clone();
        let config = config.clone();
        tokio::spawn(async move {
            if let Err(e) = subscribe_vision(config, nats).await {
                tracing::error!("Vision subscriber exited: {}", e);
            }
        });
    }

    // Full pipeline subscriber
    {
        let nats = nats.clone();
        let config = config.clone();
        let dm = dialogue_manager.clone();
        tokio::spawn(async move {
            if let Err(e) = subscribe_pipeline(config, nats, dm).await {
                tracing::error!("Pipeline subscriber exited: {}", e);
            }
        });
    }

    Ok(())
}

// ── Individual subscribers ─────────────────────────────────────

async fn subscribe_asr(config: Config, nats: Client) -> Result<(), crate::error::Error> {
    let mut sub = nats
        .subscribe("internal.ai.asr")
        .await
        .map_err(|e| crate::error::Error::Nats(e.to_string()))?;

    tracing::info!("subscribed to internal.ai.asr");

    while let Some(msg) = sub.next().await {
        let request_id = extract_request_id(&msg);
        let audio_data = msg.payload.to_vec();

        match asr::transcribe(&config, &audio_data).await {
            Ok(result) => {
                publish_result(&nats, "internal.ai.result", &request_id, &result).await;
            }
            Err(e) => {
                tracing::error!("ASR failed: {}", e);
            }
        }
    }

    Ok(())
}

async fn subscribe_nlu(config: Config, nats: Client) -> Result<(), crate::error::Error> {
    let mut sub = nats
        .subscribe("internal.ai.nlu")
        .await
        .map_err(|e| crate::error::Error::Nats(e.to_string()))?;

    tracing::info!("subscribed to internal.ai.nlu");

    while let Some(msg) = sub.next().await {
        let request_id = extract_request_id(&msg);

        let payload: Value = match serde_json::from_slice(&msg.payload) {
            Ok(v) => v,
            Err(e) => {
                tracing::warn!("invalid NLU payload: {}", e);
                continue;
            }
        };

        let text = payload["text"].as_str().unwrap_or("");

        match nlu::understand(&config, text).await {
            Ok(result) => {
                publish_result(&nats, "internal.ai.result", &request_id, &result).await;
            }
            Err(e) => {
                tracing::error!("NLU failed: {}", e);
            }
        }
    }

    Ok(())
}

async fn subscribe_dialogue(
    config: Config,
    nats: Client,
    manager: Arc<DialogueManager>,
) -> Result<(), crate::error::Error> {
    let mut sub = nats
        .subscribe("internal.ai.dialogue")
        .await
        .map_err(|e| crate::error::Error::Nats(e.to_string()))?;

    tracing::info!("subscribed to internal.ai.dialogue");

    while let Some(msg) = sub.next().await {
        let request_id = extract_request_id(&msg);

        let req: DialogueRequest = match serde_json::from_slice(&msg.payload) {
            Ok(v) => v,
            Err(e) => {
                tracing::warn!("invalid dialogue payload: {}", e);
                continue;
            }
        };

        match manager.process(&config, &req).await {
            Ok(response) => {
                publish_result(&nats, "internal.ai.result", &request_id, &response).await;

                // If the response contains text, also publish to robot command
                if !response.text.is_empty() {
                    let cmd = serde_json::json!({
                        "cmd_type": "speak",
                        "params": {"text": response.text},
                    });
                    publish_to_robot(&nats, &request_id, &cmd).await;
                }
            }
            Err(e) => {
                tracing::error!("Dialogue failed: {}", e);
            }
        }
    }

    Ok(())
}

async fn subscribe_vision(config: Config, nats: Client) -> Result<(), crate::error::Error> {
    let mut sub = nats
        .subscribe("internal.ai.vision")
        .await
        .map_err(|e| crate::error::Error::Nats(e.to_string()))?;

    tracing::info!("subscribed to internal.ai.vision");

    while let Some(msg) = sub.next().await {
        let request_id = extract_request_id(&msg);
        let image_data = msg.payload.to_vec();

        match vision::analyze(&config, &image_data).await {
            Ok(result) => {
                publish_result(&nats, "internal.ai.result", &request_id, &result).await;
            }
            Err(e) => {
                tracing::error!("Vision failed: {}", e);
            }
        }
    }

    Ok(())
}

/// Full pipeline: ASR → NLU → Dialogue → Behavior → Commands.
async fn subscribe_pipeline(
    config: Config,
    nats: Client,
    manager: Arc<DialogueManager>,
) -> Result<(), crate::error::Error> {
    let mut sub = nats
        .subscribe("internal.ai.behavior")
        .await
        .map_err(|e| crate::error::Error::Nats(e.to_string()))?;

    tracing::info!("subscribed to internal.ai.behavior");

    while let Some(msg) = sub.next().await {
        let request_id = extract_request_id(&msg);

        let payload: Value = match serde_json::from_slice(&msg.payload) {
            Ok(v) => v,
            Err(e) => {
                tracing::warn!("invalid behavior pipeline payload: {}", e);
                continue;
            }
        };

        let text = payload["text"].as_str().unwrap_or("");
        let session_id = payload["session_id"].as_str().unwrap_or("default");

        // Step 1: ASR (if audio provided)
        // (handled separately — here we assume text is already transcribed)

        // Step 2: NLU
        let nlu_result = if !text.is_empty() {
            nlu::understand(&config, text).await.ok()
        } else {
            None
        };

        // Step 3: Dialogue
        let dialogue_result = if !text.is_empty() {
            let req = DialogueRequest {
                session_id: session_id.to_string(),
                text: text.to_string(),
                intent: nlu_result.as_ref().map(|n| n.intent.clone()),
                robot_name: None,
                context: None,
            };
            manager.process(&config, &req).await.ok()
        } else {
            None
        };

        // Step 4: Behavior decision
        let decision = behavior::decide(
            &config,
            nlu_result.as_ref(),
            None, // vision result not available in pipeline
            dialogue_result.as_ref(),
        )
        .await;

        // Publish result
        publish_result(&nats, "internal.ai.result", &request_id, &decision).await;

        // Publish robot commands
        for cmd in &decision.commands {
            publish_to_robot(&nats, &request_id, cmd).await;
        }
    }

    Ok(())
}

// ── Helpers ────────────────────────────────────────────────────

fn extract_request_id(msg: &async_nats::Message) -> String {
    msg.reply
        .as_ref()
        .map(|s| s.to_string())
        .unwrap_or_else(|| uuid::Uuid::new_v4().to_string())
}

async fn publish_result(nats: &Client, subject: &str, request_id: &str, result: &impl serde::Serialize) {
    let payload = serde_json::to_vec(result).unwrap();
    let reply = request_id.to_string();

    if let Err(e) = nats.publish_with_reply(subject.to_string(), reply, payload.into()).await {
        tracing::error!("failed to publish result: {}", e);
    }
}

async fn publish_to_robot(nats: &Client, request_id: &str, cmd: &impl serde::Serialize) {
    let payload = serde_json::to_vec(cmd).unwrap();
    // In production: the scheduler determines which robot to send to.
    // Here we publish to a general subject for the gateway to forward.
    let subject = format!("robot.cmd.{}", request_id);
    if let Err(e) = nats.publish(subject, payload.into()).await {
        tracing::error!("failed to publish robot command: {}", e);
    }
}
