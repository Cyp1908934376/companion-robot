//! Gateway error types.

use bcp_core::BcpError;

/// Top-level gateway error.
#[derive(Debug, thiserror::Error)]
#[allow(dead_code)]
pub enum GatewayError {
    #[error("authentication failed: {0}")]
    AuthFailed(String),

    #[error("connection limit reached (max {0})")]
    ConnectionLimit(usize),

    #[error("robot not found: short_id={0}")]
    RobotNotFound(u16),

    #[error("rate limited")]
    RateLimited,

    #[error("NATS error: {0}")]
    NatsError(String),

    #[error("WebSocket error: {0}")]
    WsError(String),

    #[error("BCP protocol error: {0}")]
    BcpError(#[from] BcpError),

    #[error("IO error: {0}")]
    IoError(#[from] std::io::Error),

    #[error("serde error: {0}")]
    SerdeError(#[from] serde_json::Error),

    #[error(transparent)]
    Anyhow(#[from] anyhow::Error),
}

impl From<tokio_tungstenite::tungstenite::Error> for GatewayError {
    fn from(e: tokio_tungstenite::tungstenite::Error) -> Self {
        GatewayError::WsError(e.to_string())
    }
}

/// Result alias for gateway operations.
pub type Result<T> = std::result::Result<T, GatewayError>;
