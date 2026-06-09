/// Prometheus metrics for the AI service.
use lazy_static::lazy_static;
use prometheus::{register_histogram, register_int_counter, register_int_gauge, Histogram, IntCounter, IntGauge};

lazy_static! {
    // ASR
    pub static ref ASR_REQUESTS: IntCounter = register_int_counter!(
        "ai_asr_requests_total",
        "Total ASR requests"
    ).unwrap();

    pub static ref ASR_LATENCY: Histogram = register_histogram!(
        "ai_asr_latency_ms",
        "ASR latency in milliseconds",
        vec![50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0],
    ).unwrap();

    // NLU
    pub static ref NLU_REQUESTS: IntCounter = register_int_counter!(
        "ai_nlu_requests_total",
        "Total NLU requests"
    ).unwrap();

    pub static ref NLU_LATENCY: Histogram = register_histogram!(
        "ai_nlu_latency_ms",
        "NLU latency in milliseconds",
        vec![1.0, 5.0, 10.0, 25.0, 50.0, 100.0, 500.0, 1000.0],
    ).unwrap();

    // Dialogue
    pub static ref DIALOGUE_REQUESTS: IntCounter = register_int_counter!(
        "ai_dialogue_requests_total",
        "Total dialogue requests"
    ).unwrap();

    pub static ref DIALOGUE_LATENCY: Histogram = register_histogram!(
        "ai_dialogue_latency_ms",
        "Dialogue latency in milliseconds",
        vec![10.0, 50.0, 100.0, 500.0, 1000.0, 3000.0, 5000.0, 10000.0],
    ).unwrap();

    // Vision
    pub static ref VISION_REQUESTS: IntCounter = register_int_counter!(
        "ai_vision_requests_total",
        "Total vision requests"
    ).unwrap();

    pub static ref VISION_LATENCY: Histogram = register_histogram!(
        "ai_vision_latency_ms",
        "Vision latency in milliseconds",
        vec![10.0, 25.0, 50.0, 100.0, 200.0, 500.0, 1000.0],
    ).unwrap();

    // Circuit breaker
    pub static ref CIRCUIT_BREAKER_OPEN: IntGauge = register_int_gauge!(
        "ai_circuit_breaker_open",
        "Circuit breaker state (1=open, 0=closed)"
    ).unwrap();
}
