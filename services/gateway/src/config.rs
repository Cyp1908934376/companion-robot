//! Gateway service configuration.

use serde::Deserialize;

#[derive(Debug, Clone, Deserialize)]
#[allow(dead_code)]
pub struct Config {
    /// WebSocket listen address, e.g. "0.0.0.0:8080"
    #[serde(default = "default_ws_addr")]
    pub ws_addr: String,

    /// TCP listen address for direct robot connections (optional)
    #[serde(default = "default_tcp_addr")]
    pub tcp_addr: String,

    /// NATS server URL
    #[serde(default = "default_nats_url")]
    pub nats_url: String,

    /// Authentication timeout in seconds
    #[serde(default = "default_auth_timeout_secs")]
    pub auth_timeout_secs: u64,

    /// WebSocket ping interval in seconds
    #[serde(default = "default_ws_ping_secs")]
    pub ws_ping_secs: u64,

    /// Maximum connections allowed
    #[serde(default = "default_max_connections")]
    pub max_connections: usize,

    /// Per-connection rate limit (messages/second)
    #[serde(default = "default_per_conn_rate")]
    pub per_conn_rate: u32,

    /// Global rate limit (messages/second)
    #[serde(default = "default_global_rate")]
    pub global_rate: u32,

    /// Metrics HTTP listen address
    #[serde(default = "default_metrics_addr")]
    pub metrics_addr: String,

    /// Path to the robot key store JSON file (empty = dev mode, auto-authorize)
    #[serde(default = "default_key_store_path")]
    pub key_store_path: String,
}

fn default_ws_addr() -> String {
    "0.0.0.0:8080".into()
}

fn default_tcp_addr() -> String {
    "0.0.0.0:9000".into()
}

fn default_nats_url() -> String {
    "nats://localhost:4222".into()
}

fn default_auth_timeout_secs() -> u64 {
    10
}

fn default_ws_ping_secs() -> u64 {
    30
}

fn default_max_connections() -> usize {
    1000
}

fn default_per_conn_rate() -> u32 {
    100
}

fn default_global_rate() -> u32 {
    10000
}

fn default_metrics_addr() -> String {
    "0.0.0.0:9090".into()
}

fn default_key_store_path() -> String {
    String::new() // empty = dev mode
}

impl Config {
    /// Load config from environment variables (prefixed with GATEWAY_).
    pub fn from_env() -> Result<Self, anyhow::Error> {
        let mut cfg = Config::default();
        if let Ok(addr) = std::env::var("GATEWAY_WS_ADDR") {
            cfg.ws_addr = addr;
        }
        if let Ok(addr) = std::env::var("GATEWAY_TCP_ADDR") {
            cfg.tcp_addr = addr;
        }
        if let Ok(url) = std::env::var("GATEWAY_NATS_URL") {
            cfg.nats_url = url;
        }
        if let Ok(v) = std::env::var("GATEWAY_MAX_CONNS") {
            cfg.max_connections = v.parse()?;
        }
        if let Ok(path) = std::env::var("GATEWAY_KEY_STORE") {
            cfg.key_store_path = path;
        }
        Ok(cfg)
    }
}

impl Default for Config {
    fn default() -> Self {
        Config {
            ws_addr: default_ws_addr(),
            tcp_addr: default_tcp_addr(),
            nats_url: default_nats_url(),
            auth_timeout_secs: default_auth_timeout_secs(),
            ws_ping_secs: default_ws_ping_secs(),
            max_connections: default_max_connections(),
            per_conn_rate: default_per_conn_rate(),
            global_rate: default_global_rate(),
            metrics_addr: default_metrics_addr(),
            key_store_path: default_key_store_path(),
        }
    }
}
