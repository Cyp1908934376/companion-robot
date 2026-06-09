/// Scheduler error types.
use axum::http::StatusCode;
use axum::response::{IntoResponse, Response};

#[derive(Debug, thiserror::Error)]
#[allow(dead_code)]
pub enum Error {
    #[error("database error: {0}")]
    Database(#[from] sqlx::Error),

    #[error("redis error: {0}")]
    Redis(#[from] redis::RedisError),

    #[error("NATS error: {0}")]
    Nats(String),

    #[error("not found: {0}")]
    NotFound(String),

    #[error("invalid input: {0}")]
    InvalidInput(String),

    #[error("no available robots")]
    NoAvailableRobots,

    #[error("serde error: {0}")]
    Serde(#[from] serde_json::Error),

    #[error("task timeout")]
    TaskTimeout,
}

impl IntoResponse for Error {
    fn into_response(self) -> Response {
        let (status, msg) = match &self {
            Error::NotFound(_) => (StatusCode::NOT_FOUND, self.to_string()),
            Error::InvalidInput(_) => (StatusCode::BAD_REQUEST, self.to_string()),
            Error::NoAvailableRobots => (StatusCode::SERVICE_UNAVAILABLE, self.to_string()),
            _ => (StatusCode::INTERNAL_SERVER_ERROR, self.to_string()),
        };
        (status, msg).into_response()
    }
}
