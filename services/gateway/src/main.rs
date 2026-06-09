//! BCP Gateway Service — entry point.
//!
//! Starts the WebSocket gateway that accepts robot connections,
//! authenticates them, and bridges BCP frames to/from the NATS message bus.

mod auth;
mod config;
mod conn_pool;
mod error;
mod metrics;
mod nats_bridge;
mod rate_limiter;
mod ws_server;

use tracing_subscriber::EnvFilter;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    // Initialize tracing
    tracing_subscriber::fmt()
        .with_env_filter(
            EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| EnvFilter::new("gateway=info,info")),
        )
        .init();

    tracing::info!("starting BCP Gateway Service");

    // Load configuration
    let config = config::Config::from_env()?;
    // Load robot key store
    let key_store = if config.key_store_path.is_empty() {
        tracing::warn!("no key store configured, using dev mode (auto-authorize)");
        std::sync::Arc::new(auth::RobotKeyStore::dev_mode())
    } else {
        std::sync::Arc::new(
            auth::RobotKeyStore::load(&config.key_store_path)
                .expect("failed to load robot key store"),
        )
    };

    tracing::info!(
        ws_addr = %config.ws_addr,
        nats_url = %config.nats_url,
        max_connections = config.max_connections,
        authorized_robots = key_store.count(),
        "configuration loaded"
    );

    // Start Prometheus metrics HTTP endpoint
    let metrics_addr = config.metrics_addr.clone();
    tokio::spawn(async move {
        use prometheus::{Encoder, TextEncoder};
        let encoder = TextEncoder::new();
        let listener = tokio::net::TcpListener::bind(&metrics_addr).await.unwrap();
        tracing::info!(addr = %metrics_addr, "metrics endpoint listening");

        loop {
            let (stream, _) = listener.accept().await.unwrap();
            let metric_families = prometheus::gather();
            let mut buffer = Vec::new();
            encoder.encode(&metric_families, &mut buffer).unwrap();

            tokio::spawn(async move {
                use tokio::io::AsyncWriteExt;
                let (mut stream, buffer) = (stream, buffer);
                let response = format!(
                    "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: {}\r\n\r\n",
                    buffer.len()
                );
                let mut body = response.into_bytes();
                body.extend_from_slice(&*buffer);
                let _ = stream.write_all(&body).await;
            });
        }
    });

    // Run the WebSocket server (blocks until shutdown)
    ws_server::run(config, key_store).await?;

    Ok(())
}
