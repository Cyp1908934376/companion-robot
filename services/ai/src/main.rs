/// AI Inference Service — ASR / NLU / Dialogue / Vision / Behavior.
///
/// Architecture:
///   - Subscribes to internal.ai.* NATS subjects for inference requests
///   - Processes each request with the configured backend (local or API)
///   - Publishes results to internal.ai.result
///   - Dialogue manager with session state and circuit breaker
///
/// Startup:
///   1. Load configuration
///   2. Connect to NATS
///   3. Start dialogue session cleanup loop
///   4. Subscribe to all AI NATS subjects
///   5. Start HTTP health + metrics servers

mod asr;
mod behavior;
mod config;
mod dialogue;
mod error;
mod metrics;
mod nats;
mod nlu;
mod vision;

use config::Config;
use dialogue::DialogueManager;
use std::sync::Arc;
use tracing_subscriber::{fmt, prelude::*, EnvFilter};

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    tracing_subscriber::registry()
        .with(fmt::layer())
        .with(EnvFilter::try_from_default_env()
            .unwrap_or_else(|_| EnvFilter::new("info")))
        .init();

    tracing::info!("AI Inference Service starting...");

    let config = Config::from_env();
    tracing::info!("config: {:?}", config);

    // Connect to NATS
    let nats_client = async_nats::connect(&config.nats_url).await?;
    tracing::info!("connected to NATS");

    // Initialize dialogue manager
    let dialogue_manager = Arc::new(DialogueManager::new());

    // Start dialogue session cleanup
    {
        let dm = dialogue_manager.clone();
        tokio::spawn(async move {
            dialogue::run_cleanup_loop(dm).await;
        });
    }

    // Subscribe to all AI NATS subjects
    nats::subscribe_all(config.clone(), nats_client.clone(), dialogue_manager).await?;

    // Start Prometheus metrics server
    {
        let metrics_addr = config.metrics_addr.clone();
        tokio::spawn(async move {
            use prometheus::{Encoder, TextEncoder};
            use axum::{routing::get, Router};

            let app = Router::new().route("/metrics", get(|| async {
                let encoder = TextEncoder::new();
                let metric_families = prometheus::gather();
                let mut buffer = Vec::new();
                encoder.encode(&metric_families, &mut buffer).unwrap();
                String::from_utf8(buffer).unwrap()
            }));

            let listener = tokio::net::TcpListener::bind(&metrics_addr).await.unwrap();
            tracing::info!("metrics server on {}", metrics_addr);
            axum::serve(listener, app).await.unwrap();
        });
    }

    // Start health check HTTP server
    {
        let http_addr = config.http_addr.clone();
        tokio::spawn(async move {
            use axum::{routing::get, Router};

            let app = Router::new()
                .route("/health", get(|| async { "OK" }));

            let listener = tokio::net::TcpListener::bind(&http_addr).await.unwrap();
            tracing::info!("AI service health on {}", http_addr);
            axum::serve(listener, app).await.unwrap();
        });
    }

    // Keep main task alive
    tokio::signal::ctrl_c().await?;
    tracing::info!("shutting down");

    Ok(())
}
