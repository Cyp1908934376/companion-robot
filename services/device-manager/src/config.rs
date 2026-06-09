/// Device Manager configuration.
///
/// Reads from environment variables with DEVICE_MGR_ prefix.
#[derive(Debug, Clone)]
pub struct Config {
    pub http_addr: String,
    pub metrics_addr: String,
    pub database_url: String,
    pub redis_url: String,
    pub nats_url: String,
    pub heartbeat_timeout_secs: u64,
    pub heartbeat_check_interval_secs: u64,
}

impl Config {
    pub fn from_env() -> Self {
        Self {
            http_addr: std::env::var("DEVICE_MGR_HTTP_ADDR")
                .unwrap_or_else(|_| "0.0.0.0:8081".into()),
            metrics_addr: std::env::var("DEVICE_MGR_METRICS_ADDR")
                .unwrap_or_else(|_| "0.0.0.0:9181".into()),
            database_url: std::env::var("DEVICE_MGR_DATABASE_URL")
                .unwrap_or_else(|_| "postgres://postgres:postgres@localhost:5432/companion".into()),
            redis_url: std::env::var("DEVICE_MGR_REDIS_URL")
                .unwrap_or_else(|_| "redis://localhost:6379".into()),
            nats_url: std::env::var("DEVICE_MGR_NATS_URL")
                .unwrap_or_else(|_| "nats://localhost:4222".into()),
            heartbeat_timeout_secs: std::env::var("DEVICE_MGR_HEARTBEAT_TIMEOUT")
                .ok()
                .and_then(|v| v.parse().ok())
                .unwrap_or(15),
            heartbeat_check_interval_secs: std::env::var("DEVICE_MGR_HEARTBEAT_CHECK_INTERVAL")
                .ok()
                .and_then(|v| v.parse().ok())
                .unwrap_or(5),
        }
    }
}
