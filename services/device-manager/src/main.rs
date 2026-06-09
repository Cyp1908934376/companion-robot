/// Device Manager — robot registry, heartbeat monitoring, REST API.
///
/// Startup:
///   1. Load configuration from environment
///   2. Initialize PostgreSQL connection pool and run migrations
///   3. Connect to Redis
///   4. Connect to NATS and subscribe to robot events
///   5. Start heartbeat monitor
///   6. Start HTTP API server
///   7. Start Prometheus metrics server

mod config;
mod db;
mod error;
mod heartbeat;
mod metrics;
mod nats;
mod api;
mod redis;

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

    tracing::info!("Device Manager starting...");

    // Load config
    let config = Config::from_env();
    tracing::info!("config: {:?}", config);

    // PostgreSQL connection pool
    let pool = PgPoolOptions::new()
        .max_connections(10)
        .connect(&config.database_url)
        .await?;

    tracing::info!("connected to PostgreSQL");

    // Run migrations
    db::init_db(&pool).await?;

    // Redis connection
    let redis_conn = redis::connect(&config.redis_url).await?;

    // NATS connection
    let nats_client = async_nats::connect(&config.nats_url).await?;
    tracing::info!("connected to NATS");

    // Subscribe to NATS events
    nats::subscribe_all(nats_client.clone(), pool.clone(), redis_conn.clone()).await?;

    // Start heartbeat monitor
    let hb_pool = pool.clone();
    let hb_redis = redis_conn.clone();
    let hb_nats = nats_client.clone();
    let hb_config = config.clone();

    tokio::spawn(async move {
        heartbeat::run(hb_config, hb_pool, hb_redis, hb_nats).await;
    });

    // Start Prometheus metrics server
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

    // Start HTTP API server
    let app_state = api::AppState::new(pool, redis_conn);
    let app = api::router(app_state);

    let listener = tokio::net::TcpListener::bind(&config.http_addr).await?;
    tracing::info!("Device Manager listening on {}", config.http_addr);

    axum::serve(listener, app).await?;

    Ok(())
}
