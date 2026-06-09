/// Scheduler — multi-robot task assignment with weighted scoring.
///
/// Startup:
///   1. Load configuration from environment
///   2. Initialize PostgreSQL connection pool, run migrations
///   3. Connect to NATS, subscribe to task status + device status
///   4. Connect to Redis (for fast robot state queries)
///   5. Start task dispatch loop (continuous assignment)
///   6. Start HTTP API server
///   7. Start Prometheus metrics server

mod api;
mod config;
mod error;
mod metrics;
mod nats;
mod scheduler;
mod task;

use config::Config;
use sqlx::postgres::PgPoolOptions;
use tracing_subscriber::{fmt, prelude::*, EnvFilter};

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    // Initialize tracing
    tracing_subscriber::registry()
        .with(fmt::layer())
        .with(EnvFilter::try_from_default_env()
            .unwrap_or_else(|_| EnvFilter::new("info")))
        .init();

    tracing::info!("Scheduler starting...");

    // Load config
    let config = Config::from_env();
    tracing::info!("config: {:?}", config);

    // PostgreSQL
    let pool = PgPoolOptions::new()
        .max_connections(10)
        .connect(&config.database_url)
        .await?;

    tracing::info!("connected to PostgreSQL");

    // Run migrations
    scheduler::init_db(&pool).await?;

    // NATS
    let nats_client = async_nats::connect(&config.nats_url).await?;
    tracing::info!("connected to NATS");

    // Subscribe to task status updates from robots
    {
        let nats = nats_client.clone();
        let pool = pool.clone();
        tokio::spawn(async move {
            if let Err(e) = nats::subscribe_task_status(nats, pool).await {
                tracing::error!("task status subscriber exited: {}", e);
            }
        });
    }

    // Subscribe to device status changes
    {
        let nats = nats_client.clone();
        let pool = pool.clone();
        tokio::spawn(async move {
            if let Err(e) = nats::subscribe_device_status(nats, pool).await {
                tracing::error!("device status subscriber exited: {}", e);
            }
        });
    }

    // Start task dispatch loop
    {
        let config = config.clone();
        let pool = pool.clone();
        let nats = nats_client.clone();
        tokio::spawn(async move {
            scheduler::run_dispatch_loop(config, pool, nats).await;
        });
    }

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

    // Start HTTP API server
    let app_state = api::AppState {
        pool: pool.clone(),
        nats: nats_client,
    };
    let app = api::router(app_state);

    let listener = tokio::net::TcpListener::bind(&config.http_addr).await?;
    tracing::info!("Scheduler listening on {}", config.http_addr);

    axum::serve(listener, app).await?;

    Ok(())
}
