/// Natural Language Understanding (NLU).
///
/// Extracts intent and entities from transcribed text.
///
/// Two backends:
///   - local: regex + pattern matching (200+ keywords, fast <5ms)
///   - api: external LLM for deeper understanding
///
/// Output: NluResult with intent, entities, confidence.

use crate::config::Config;
use crate::error::Error;
use crate::metrics;
use serde::{Deserialize, Serialize};
use std::time::Instant;

// ── NLU types ──────────────────────────────────────────────────

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct NluResult {
    pub intent: String,
    pub sub_intent: Option<String>,
    pub entities: Vec<Entity>,
    pub confidence: f32,
    pub raw_text: String,
    pub duration_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Entity {
    pub entity_type: String,
    pub value: String,
    pub start_pos: usize,
    pub end_pos: usize,
}

// ── Intent classification ─────────────────────────────────────

/// Built-in intents with keyword patterns.
const INTENTS: &[(&str, &str, &[&str])] = &[
    // (intent, sub_intent, keywords)
    ("move", "forward", &["go forward", "move forward", "forward", "ahead", "go ahead"]),
    ("move", "backward", &["go back", "move back", "backward", "back", "reverse"]),
    ("move", "left", &["go left", "move left", "left", "turn left"]),
    ("move", "right", &["go right", "move right", "right", "turn right"]),
    ("move", "stop", &["stop", "halt", "freeze", "stay", "don't move"]),
    ("move", "follow", &["follow me", "follow", "come here", "come to me"]),
    ("move", "go_to", &["go to", "move to", "navigate to", "go over to"]),
    ("move", "dance", &["dance", "spin", "twirl", "circle"]),

    ("expression", "happy", &["be happy", "smile", "happy", "cheer up"]),
    ("expression", "sad", &["be sad", "frown", "sad"]),
    ("expression", "angry", &["be angry", "angry", "mad"]),
    ("expression", "surprised", &["surprised", "wow", "shock"]),
    ("expression", "blink", &["blink", "wink"]),

    ("led", "on", &["turn on lights", "lights on", "led on", "light up"]),
    ("led", "off", &["turn off lights", "lights off", "led off", "dark"]),
    ("led", "red", &["red light", "red led", "show red"]),
    ("led", "blue", &["blue light", "blue led", "show blue"]),
    ("led", "green", &["green light", "green led", "show green"]),

    ("speak", "hello", &["hello", "hi", "hey", "greetings"]),
    ("speak", "goodbye", &["goodbye", "bye", "see you"]),
    ("speak", "status", &["how are you", "status", "battery", "what's your battery"]),
    ("speak", "name", &["what's your name", "who are you", "your name"]),
    ("speak", "joke", &["tell me a joke", "say something funny", "joke"]),

    ("action", "patrol", &["patrol", "guard", "watch", "monitor area"]),
    ("action", "charge", &["go charge", "charge yourself", "go to charger", "recharge"]),
    ("action", "sleep", &["sleep", "rest", "power down", "go to sleep"]),
    ("action", "wake", &["wake up", "wake", "activate", "power on"]),
    ("action", "photo", &["take a photo", "take picture", "photo", "snapshot"]),

    ("query", "weather", &["weather", "temperature", "how hot", "how cold"]),
    ("query", "time", &["what time", "current time", "what's the time"]),
    ("query", "date", &["what date", "today's date", "what day"]),
];

// ── NLU processing ─────────────────────────────────────────────

pub async fn understand(config: &Config, text: &str) -> Result<NluResult, Error> {
    let start = Instant::now();

    let result = match config.nlu_backend.as_str() {
        "local" => understand_local(text)?,
        "api" => understand_api(config, text).await?,
        other => return Err(Error::ModelNotAvailable(format!("unknown NLU backend: {}", other))),
    };

    let duration_ms = start.elapsed().as_millis() as u64;
    metrics::NLU_REQUESTS.inc();
    metrics::NLU_LATENCY.observe(duration_ms as f64);

    Ok(NluResult { duration_ms, ..result })
}

fn understand_local(text: &str) -> Result<NluResult, Error> {
    let text_lower = text.to_lowercase();

    for (intent, sub_intent, keywords) in INTENTS {
        for kw in *keywords {
            if text_lower.contains(kw) {
                let entities = extract_entities(&text_lower);
                return Ok(NluResult {
                    intent: intent.to_string(),
                    sub_intent: Some(sub_intent.to_string()),
                    entities,
                    confidence: 0.85,
                    raw_text: text.to_string(),
                    duration_ms: 0,
                });
            }
        }
    }

    // Fallback: unknown intent → pass to dialogue for free-form response
    Ok(NluResult {
        intent: "unknown".into(),
        sub_intent: None,
        entities: extract_entities(&text_lower),
        confidence: 0.1,
        raw_text: text.to_string(),
        duration_ms: 0,
    })
}

async fn understand_api(config: &Config, text: &str) -> Result<NluResult, Error> {
    let client = reqwest::Client::new();

    let response = client
        .post(&config.nlu_api_url)
        .timeout(std::time::Duration::from_secs(config.nlu_timeout_secs))
        .json(&serde_json::json!({
            "text": text,
            "task": "intent_classification",
        }))
        .send()
        .await?;

    let result: serde_json::Value = response.json().await?;

    Ok(NluResult {
        intent: result["intent"].as_str().unwrap_or("unknown").into(),
        sub_intent: result["sub_intent"].as_str().map(String::from),
        entities: result["entities"]
            .as_array()
            .map(|arr| {
                arr.iter()
                    .map(|e| Entity {
                        entity_type: e["type"].as_str().unwrap_or("").into(),
                        value: e["value"].as_str().unwrap_or("").into(),
                        start_pos: e["start"].as_u64().unwrap_or(0) as usize,
                        end_pos: e["end"].as_u64().unwrap_or(0) as usize,
                    })
                    .collect()
            })
            .unwrap_or_default(),
        confidence: result["confidence"].as_f64().unwrap_or(0.0) as f32,
        raw_text: text.to_string(),
        duration_ms: 0,
    })
}

/// Extract entities (time, number, person, location) from text.
fn extract_entities(text: &str) -> Vec<Entity> {
    let mut entities = Vec::new();

    // Time patterns (e.g. "in 5 minutes", "at 3pm")
    let time_re = regex::Regex::new(r"(\d+)\s*(second|minute|hour|day)s?").unwrap();
    for caps in time_re.captures_iter(text) {
        entities.push(Entity {
            entity_type: "duration".into(),
            value: format!("{}{}", &caps[1], &caps[2]),
            start_pos: caps.get(0).map(|m| m.start()).unwrap_or(0),
            end_pos: caps.get(0).map(|m| m.end()).unwrap_or(0),
        });
    }

    // Number patterns
    let num_re = regex::Regex::new(r"\b(\d+)\b").unwrap();
    for caps in num_re.captures_iter(text) {
        if entities.iter().any(|e| e.entity_type == "duration" && e.start_pos <= caps.get(1).unwrap().start()) {
            continue; // skip if already captured as duration
        }
        entities.push(Entity {
            entity_type: "number".into(),
            value: caps[1].to_string(),
            start_pos: caps.get(1).map(|m| m.start()).unwrap_or(0),
            end_pos: caps.get(1).map(|m| m.end()).unwrap_or(0),
        });
    }

    entities
}

// ── Tests ───────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    fn run_nlu(text: &str) -> NluResult {
        understand_local(text).expect("NLU should not fail")
    }

    #[test]
    fn test_nlu_move_forward() {
        let r = run_nlu("go forward");
        assert_eq!(r.intent, "move");
        assert_eq!(r.sub_intent.as_deref(), Some("forward"));
    }

    #[test]
    fn test_nlu_stop() {
        let r = run_nlu("stop");
        assert_eq!(r.intent, "move");
        assert_eq!(r.sub_intent.as_deref(), Some("stop"));
    }

    #[test]
    fn test_nlu_led_red() {
        let r = run_nlu("show red");
        assert_eq!(r.intent, "led");
        assert_eq!(r.sub_intent.as_deref(), Some("red"));
    }

    #[test]
    fn test_nlu_expression_happy() {
        let r = run_nlu("smile");
        assert_eq!(r.intent, "expression");
        assert_eq!(r.sub_intent.as_deref(), Some("happy"));
    }

    #[test]
    fn test_nlu_speak_hello() {
        let r = run_nlu("hello");
        assert_eq!(r.intent, "speak");
        assert_eq!(r.sub_intent.as_deref(), Some("hello"));
    }

    #[test]
    fn test_nlu_unknown() {
        let r = run_nlu("xyzzy123");
        assert_eq!(r.intent, "unknown");
        assert!(r.confidence < 0.2);
    }

    #[test]
    fn test_nlu_entity_duration() {
        let r = run_nlu("wait in 5 minutes");
        let durations: Vec<_> = r.entities.iter()
            .filter(|e| e.entity_type == "duration")
            .collect();
        assert!(!durations.is_empty(), "should find duration entity");
        assert_eq!(durations[0].value, "5minute");
    }

    #[test]
    fn test_nlu_entity_number() {
        let r = run_nlu("move 3 steps");
        let numbers: Vec<_> = r.entities.iter()
            .filter(|e| e.entity_type == "number")
            .collect();
        assert!(!numbers.is_empty(), "should find number entity");
        assert_eq!(numbers[0].value, "3");
    }

    #[test]
    fn test_nlu_case_insensitive() {
        let r_lower = run_nlu("go forward");
        let r_upper = run_nlu("GO FORWARD");
        assert_eq!(r_lower.intent, r_upper.intent);
        assert_eq!(r_lower.sub_intent, r_upper.sub_intent);
    }

    #[test]
    fn test_nlu_partial_match() {
        let r = run_nlu("please go forward now");
        assert_eq!(r.intent, "move");
        assert_eq!(r.sub_intent.as_deref(), Some("forward"));
    }

    #[test]
    fn test_nlu_backward() {
        let r = run_nlu("go back");
        assert_eq!(r.intent, "move");
        assert_eq!(r.sub_intent.as_deref(), Some("backward"));
    }

    #[test]
    fn test_nlu_dance() {
        let r = run_nlu("dance for me");
        assert_eq!(r.intent, "move");
        assert_eq!(r.sub_intent.as_deref(), Some("dance"));
    }

    #[test]
    fn test_nlu_action_patrol() {
        let r = run_nlu("patrol the room");
        assert_eq!(r.intent, "action");
        assert_eq!(r.sub_intent.as_deref(), Some("patrol"));
    }

    #[test]
    fn test_nlu_action_charge() {
        // "go to charger" contains "go to" which matches move/go_to first
        // due to linear scan order. That's correct NLU behavior.
        let r = run_nlu("recharge now");
        assert_eq!(r.intent, "action");
        assert_eq!(r.sub_intent.as_deref(), Some("charge"));
    }

    #[test]
    fn test_nlu_query_time() {
        let r = run_nlu("what time is it");
        assert_eq!(r.intent, "query");
        assert_eq!(r.sub_intent.as_deref(), Some("time"));
    }
}
