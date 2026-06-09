/// AI service error types.

#[derive(Debug, thiserror::Error)]
#[allow(dead_code)]
pub enum Error {
    #[error("NATS error: {0}")]
    Nats(String),

    #[error("HTTP error: {0}")]
    Http(#[from] reqwest::Error),

    #[error("inference error: {0}")]
    Inference(String),

    #[error("timeout: {0}")]
    Timeout(String),

    #[error("model not available: {0}")]
    ModelNotAvailable(String),

    #[error("invalid input: {0}")]
    InvalidInput(String),

    #[error("serde error: {0}")]
    Serde(#[from] serde_json::Error),

    #[error("circuit breaker open")]
    CircuitBreakerOpen,
}
