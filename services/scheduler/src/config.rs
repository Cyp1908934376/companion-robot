/// Scheduler configuration.
///
/// Reads from environment variables with SCHEDULER_ prefix.
#[derive(Debug, Clone)]
#[allow(dead_code)]
pub struct Config {
    pub http_addr: String,
    pub metrics_addr: String,
    pub database_url: String,
    pub redis_url: String,
    pub nats_url: String,
    pub max_retries: u8,
    pub default_task_timeout_secs: u64,
    pub task_cleanup_interval_secs: u64,
}

impl Config {
    pub fn from_env() -> Self {
        Self {
            http_addr: std::env::var("SCHEDULER_HTTP_ADDR")
                .unwrap_or_else(|_| "0.0.0.0:8082".into()),
            metrics_addr: std::env::var("SCHEDULER_METRICS_ADDR")
                .unwrap_or_else(|_| "0.0.0.0:9182".into()),
            database_url: std::env::var("SCHEDULER_DATABASE_URL")
                .unwrap_or_else(|_| "postgres://postgres:postgres@localhost:5432/companion".into()),
            redis_url: std::env::var("SCHEDULER_REDIS_URL")
                .unwrap_or_else(|_| "redis://localhost:6379".into()),
            nats_url: std::env::var("SCHEDULER_NATS_URL")
                .unwrap_or_else(|_| "nats://localhost:4222".into()),
            max_retries: std::env::var("SCHEDULER_MAX_RETRIES")
                .ok()
                .and_then(|v| v.parse().ok())
                .unwrap_or(3),
            default_task_timeout_secs: std::env::var("SCHEDULER_TASK_TIMEOUT")
                .ok()
                .and_then(|v| v.parse().ok())
                .unwrap_or(30),
            task_cleanup_interval_secs: std::env::var("SCHEDULER_CLEANUP_INTERVAL")
                .ok()
                .and_then(|v| v.parse().ok())
                .unwrap_or(10),
        }
    }
}
