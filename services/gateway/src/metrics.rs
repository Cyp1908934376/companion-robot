//! Prometheus metrics for the gateway service.

use lazy_static::lazy_static;
use prometheus::{
    register_histogram, register_int_counter, register_int_counter_vec, register_int_gauge,
    Histogram, IntCounter, IntCounterVec, IntGauge,
};

lazy_static! {
    /// Number of active WebSocket/TCP connections.
    pub static ref CONNECTIONS: IntGauge =
        register_int_gauge!("gateway_connections", "Active connections").unwrap();

    /// Total connection attempts.
    pub static ref CONNECTION_ATTEMPTS: IntCounter =
        register_int_counter!("gateway_connection_attempts_total", "Total connection attempts").unwrap();

    /// Failed authentication attempts by reason.
    pub static ref AUTH_FAILURES: IntCounterVec =
        register_int_counter_vec!(
            "gateway_auth_failures_total",
            "Authentication failures by reason",
            &["reason"]
        ).unwrap();

    /// Messages forwarded to NATS.
    pub static ref MESSAGES_UPLINK: IntCounter =
        register_int_counter!("gateway_messages_uplink_total", "Total uplink messages to NATS").unwrap();

    /// Messages forwarded from NATS to robots.
    pub static ref MESSAGES_DOWNLINK: IntCounter =
        register_int_counter!("gateway_messages_downlink_total", "Total downlink messages from NATS").unwrap();

    /// Rate-limited message drops.
    pub static ref RATE_LIMITED: IntCounter =
        register_int_counter!("gateway_rate_limited_total", "Messages dropped by rate limiter").unwrap();

    /// Command forwarding latency in milliseconds.
    pub static ref CMD_LATENCY: Histogram =
        register_histogram!("gateway_cmd_latency_ms", "Command forwarding latency (ms)").unwrap();

    /// NATS publish errors.
    pub static ref NATS_ERRORS: IntCounter =
        register_int_counter!("gateway_nats_errors_total", "NATS publish errors").unwrap();

    /// Robot disconnections.
    pub static ref DISCONNECTS: IntCounter =
        register_int_counter!("gateway_disconnects_total", "Robot disconnections").unwrap();
}

/// Update the active connection gauge.
#[allow(dead_code)]
pub fn set_connections(count: i64) {
    CONNECTIONS.set(count);
}

/// Record a command forwarding latency.
pub fn record_latency_ms(ms: f64) {
    CMD_LATENCY.observe(ms);
}
