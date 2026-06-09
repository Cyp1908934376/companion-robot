/// Dialogue management — conversation state and response generation.
///
/// Maintains per-session conversation context.
/// Generates responses using either:
///   - local: template-based responses (fast, deterministic)
///   - api: external LLM (Claude/GPT) for open-ended conversation
///
/// Circuit breaker: if API failure rate > 50%, switch to local for 30s.

use crate::config::Config;
use crate::error::Error;
use crate::metrics;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::sync::Arc;
use std::time::{Duration, Instant};
use tokio::sync::RwLock;

// ── Dialogue types ─────────────────────────────────────────────

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DialogueRequest {
    pub session_id: String,
    pub text: String,
    pub intent: Option<String>,
    pub robot_name: Option<String>,
    pub context: Option<HashMap<String, String>>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DialogueResponse {
    pub session_id: String,
    pub text: String,
    pub emotion: Option<String>,
    pub action: Option<DialogueAction>,
    pub duration_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DialogueAction {
    pub action_type: String,
    pub params: serde_json::Value,
}

// ── Session state ──────────────────────────────────────────────

#[derive(Debug, Clone)]
struct DialogueSession {
    pub history: Vec<Message>,
    pub last_active: Instant,
    pub robot_name: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct Message {
    pub role: String, // "user" or "assistant"
    pub content: String,
}

// ── Circuit breaker ────────────────────────────────────────────

#[derive(Debug, Clone)]
struct CircuitBreaker {
    failure_count: u32,
    success_count: u32,
    last_failure: Option<Instant>,
    open: bool,
    opened_at: Option<Instant>,
    threshold: f32,      // failure rate threshold (0.0–1.0)
    cooldown_secs: u64,
}

impl CircuitBreaker {
    fn new(threshold: f32, cooldown_secs: u64) -> Self {
        Self {
            failure_count: 0,
            success_count: 0,
            last_failure: None,
            open: false,
            opened_at: None,
            threshold,
            cooldown_secs,
        }
    }

    fn record_success(&mut self) {
        self.success_count += 1;
        self.failure_count = self.failure_count.saturating_sub(1);
    }

    fn record_failure(&mut self) {
        self.failure_count += 1;
        self.last_failure = Some(Instant::now());
        let total = self.failure_count + self.success_count;
        if total > 5 {
            let rate = self.failure_count as f32 / total as f32;
            if rate > self.threshold {
                self.open = true;
                self.opened_at = Some(Instant::now());
                tracing::warn!("circuit breaker opened (failure rate: {:.0}%)", rate * 100.0);
                metrics::CIRCUIT_BREAKER_OPEN.set(1);
            }
        }
    }

    fn is_open(&mut self) -> bool {
        if !self.open {
            return false;
        }
        // Auto-recover after cooldown
        if let Some(opened) = self.opened_at {
            if opened.elapsed() > Duration::from_secs(self.cooldown_secs) {
                self.open = false;
                self.failure_count = 0;
                self.success_count = 0;
                metrics::CIRCUIT_BREAKER_OPEN.set(0);
                tracing::info!("circuit breaker closed (cooldown elapsed)");
                return false;
            }
        }
        true
    }
}

// ── Dialogue manager ───────────────────────────────────────────

pub struct DialogueManager {
    sessions: RwLock<HashMap<String, DialogueSession>>,
    circuit_breaker: RwLock<CircuitBreaker>,
}

impl DialogueManager {
    pub fn new() -> Self {
        Self {
            sessions: RwLock::new(HashMap::new()),
            circuit_breaker: RwLock::new(CircuitBreaker::new(0.5, 30)),
        }
    }

    /// Process a dialogue turn and generate a response.
    pub async fn process(
        &self,
        config: &Config,
        req: &DialogueRequest,
    ) -> Result<DialogueResponse, Error> {
        let start = Instant::now();

        // Get or create session
        let mut sessions = self.sessions.write().await;
        let session = sessions.entry(req.session_id.clone()).or_insert_with(|| {
            DialogueSession {
                history: Vec::new(),
                last_active: Instant::now(),
                robot_name: req.robot_name.clone().unwrap_or_else(|| "Robot".into()),
            }
        });

        session.last_active = Instant::now();
        session.history.push(Message {
            role: "user".into(),
            content: req.text.clone(),
        });

        // Keep history bounded
        if session.history.len() > 20 {
            session.history.remove(0);
        }

        // Check circuit breaker
        let cb_open = self.circuit_breaker.write().await.is_open();

        let response_text = if cb_open || config.dialogue_backend == "local" {
            generate_local(&session.history, &session.robot_name)?
        } else {
            match generate_api(config, &session.history, &session.robot_name).await {
                Ok(text) => {
                    self.circuit_breaker.write().await.record_success();
                    text
                }
                Err(e) => {
                    self.circuit_breaker.write().await.record_failure();
                    tracing::warn!("API dialogue failed, fallback to local: {}", e);
                    generate_local(&session.history, &session.robot_name)?
                }
            }
        };

        // Add assistant response to history
        session.history.push(Message {
            role: "assistant".into(),
            content: response_text.clone(),
        });

        let duration_ms = start.elapsed().as_millis() as u64;
        metrics::DIALOGUE_REQUESTS.inc();
        metrics::DIALOGUE_LATENCY.observe(duration_ms as f64);

        Ok(DialogueResponse {
            session_id: req.session_id.clone(),
            text: response_text,
            emotion: None,
            action: None,
            duration_ms,
        })
    }

    /// Clean up stale sessions (inactive > 30 minutes).
    pub async fn cleanup_stale_sessions(&self) {
        let mut sessions = self.sessions.write().await;
        let stale: Vec<String> = sessions
            .iter()
            .filter(|(_, s)| s.last_active.elapsed() > Duration::from_secs(1800))
            .map(|(id, _)| id.clone())
            .collect();

        for id in stale {
            sessions.remove(&id);
            tracing::debug!("removed stale dialogue session: {}", id);
        }
    }
}

// ── Response generators ────────────────────────────────────────

fn generate_local(history: &[Message], robot_name: &str) -> Result<String, Error> {
    let last_msg = history.last().map(|m| m.content.as_str()).unwrap_or("");

    // Template-based responses
    let response = if last_msg.contains("hello") || last_msg.contains("hi ") || last_msg == "hi" {
        format!("Hello! I'm {}. How can I help you?", robot_name)
    } else if last_msg.contains("name") {
        format!("My name is {}! I'm a companion robot.", robot_name)
    } else if last_msg.contains("joke") {
        "Why did the robot go on vacation? It needed to recharge its batteries!".into()
    } else if last_msg.contains("battery") || last_msg.contains("status") {
        "I'm doing well! All systems operational. Let me check my battery level for you.".into()
    } else if last_msg.contains("bye") {
        "Goodbye! It was nice talking to you.".into()
    } else if last_msg.contains("thanks") || last_msg.contains("thank you") {
        "You're welcome! Happy to help.".into()
    } else {
        format!("I understand you said: \"{}\". I'm still learning, but I'll do my best to help!", last_msg)
    };

    Ok(response)
}

async fn generate_api(
    config: &Config,
    history: &[Message],
    _robot_name: &str,
) -> Result<String, Error> {
    let client = reqwest::Client::new();

    // Build conversation messages for the API
    let messages: Vec<serde_json::Value> = history
        .iter()
        .map(|m| {
            serde_json::json!({
                "role": m.role,
                "content": m.content,
            })
        })
        .collect();

    let response = client
        .post(&config.dialogue_api_url)
        .header("x-api-key", &config.dialogue_api_key)
        .header("anthropic-version", "2023-06-01")
        .json(&serde_json::json!({
            "model": "claude-sonnet-4-20250514",
            "max_tokens": 150,
            "messages": messages,
            "system": "You are a friendly companion robot. Keep responses short (1-2 sentences), warm, and helpful. The robot can move, express emotions via LEDs and a face display, and speaks with a voice.",
        }))
        .send()
        .await?;

    if !response.status().is_success() {
        return Err(Error::Inference(format!(
            "dialogue API returned {}",
            response.status()
        )));
    }

    let result: serde_json::Value = response.json().await?;

    // Extract response text from Claude API format
    let text = result["content"][0]["text"]
        .as_str()
        .unwrap_or("I'm not sure how to respond to that.")
        .to_string();

    Ok(text)
}

// ── Session cleanup task ───────────────────────────────────────

pub async fn run_cleanup_loop(manager: Arc<DialogueManager>) -> ! {
    loop {
        tokio::time::sleep(Duration::from_secs(600)).await; // every 10 minutes
        manager.cleanup_stale_sessions().await;
    }
}
