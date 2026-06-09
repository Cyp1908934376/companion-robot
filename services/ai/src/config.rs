/// AI service configuration.
///
/// Environment variables with AI_ prefix.
#[derive(Debug, Clone)]
#[allow(dead_code)]
pub struct Config {
    pub http_addr: String,
    pub metrics_addr: String,
    pub nats_url: String,

    // ASR
    pub asr_backend: String,          // "whisper" or "api"
    pub asr_model_path: String,       // local whisper model path
    pub asr_api_url: String,          // external ASR API URL

    // NLU
    pub nlu_backend: String,          // "local" or "api"
    pub nlu_api_url: String,

    // Dialogue
    pub dialogue_backend: String,     // "local" or "api"
    pub dialogue_api_url: String,
    pub dialogue_api_key: String,

    // Vision
    pub vision_backend: String,       // "yolo" or "api"
    pub vision_model_path: String,
    pub vision_api_url: String,
    pub vision_detection_threshold: f32,

    // Behavior
    pub behavior_default_temperature: f32,

    // Timeouts
    pub asr_timeout_secs: u64,
    pub nlu_timeout_secs: u64,
    pub vision_timeout_secs: u64,
    pub inference_timeout_secs: u64,
}

impl Config {
    pub fn from_env() -> Self {
        Self {
            http_addr: env_or("AI_HTTP_ADDR", "0.0.0.0:8083"),
            metrics_addr: env_or("AI_METRICS_ADDR", "0.0.0.0:9183"),
            nats_url: env_or("AI_NATS_URL", "nats://localhost:4222"),

            asr_backend: env_or("AI_ASR_BACKEND", "whisper"),
            asr_model_path: env_or("AI_ASR_MODEL_PATH", "./models/whisper-small.bin"),
            asr_api_url: env_or("AI_ASR_API_URL", ""),

            nlu_backend: env_or("AI_NLU_BACKEND", "local"),
            nlu_api_url: env_or("AI_NLU_API_URL", ""),

            dialogue_backend: env_or("AI_DIALOGUE_BACKEND", "api"),
            dialogue_api_url: env_or("AI_DIALOGUE_API_URL", "https://api.anthropic.com/v1/messages"),
            dialogue_api_key: env_or("AI_DIALOGUE_API_KEY", ""),

            vision_backend: env_or("AI_VISION_BACKEND", "yolo"),
            vision_model_path: env_or("AI_VISION_MODEL_PATH", "./models/yolov8n.onnx"),
            vision_api_url: env_or("AI_VISION_API_URL", ""),
            vision_detection_threshold: env_or("AI_VISION_THRESHOLD", "0.5").parse().unwrap_or(0.5),

            behavior_default_temperature: env_or("AI_BEHAVIOR_TEMPERATURE", "0.7").parse().unwrap_or(0.7),

            asr_timeout_secs: env_or("AI_ASR_TIMEOUT", "10").parse().unwrap_or(10),
            nlu_timeout_secs: env_or("AI_NLU_TIMEOUT", "5").parse().unwrap_or(5),
            vision_timeout_secs: env_or("AI_VISION_TIMEOUT", "5").parse().unwrap_or(5),
            inference_timeout_secs: env_or("AI_INFERENCE_TIMEOUT", "30").parse().unwrap_or(30),
        }
    }
}

fn env_or(key: &str, default: &str) -> String {
    std::env::var(key).unwrap_or_else(|_| default.to_string())
}
