/// Behavior decision engine — maps AI results to robot commands.
///
/// Takes NLU intent/entities + dialogue response + vision detections
/// and produces concrete robot commands (BCP instructions).
///
/// Decision modes:
///   - deterministic: rule-based command mapping
///   - generative: LLM decides the best action sequence

use crate::config::Config;
use crate::nlu::NluResult;
use crate::dialogue::DialogueResponse;
use crate::vision::VisionResult;
use serde::{Deserialize, Serialize};
use std::time::Instant;

// ── Behavior output ────────────────────────────────────────────

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct BehaviorDecision {
    pub commands: Vec<RobotCommand>,
    pub reasoning: Option<String>,
    pub should_speak: Option<String>,  // TTS text if the robot should say something
    pub emotion: Option<String>,       // face expression
    pub duration_ms: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RobotCommand {
    pub cmd_type: String,          // "move", "stop", "led", "face", "speak", "servo"
    pub params: serde_json::Value,
    pub priority: u8,              // 0=emergency, 1=motion, 2=expression, 3=best_effort
    pub duration_ms: Option<u32>,
}

// ── Decision engine ────────────────────────────────────────────

pub async fn decide(
    _config: &Config,
    nlu: Option<&NluResult>,
    vision: Option<&VisionResult>,
    dialogue: Option<&DialogueResponse>,
) -> BehaviorDecision {
    let start = Instant::now();

    let mut commands = Vec::new();
    let mut should_speak = None;
    let mut emotion = None;

    // 1. React to obstacles first (highest priority)
    if let Some(vision) = vision {
        if vision.person_count > 0 {
            commands.push(RobotCommand {
                cmd_type: "led".into(),
                params: serde_json::json!({"mode": "breathing", "color": {"r": 0, "g": 255, "b": 0}}),
                priority: 2,
                duration_ms: Some(2000),
            });
        }
    }

    // 2. React to NLU intent
    if let Some(nlu) = nlu {
        let intent_cmds = map_intent_to_commands(nlu);
        commands.extend(intent_cmds);
    }

    // 3. Dialogue response → TTS + expression
    if let Some(dialogue) = dialogue {
        should_speak = Some(dialogue.text.clone());
        emotion = dialogue.emotion.clone();

        commands.push(RobotCommand {
            cmd_type: "speak".into(),
            params: serde_json::json!({"text": dialogue.text}),
            priority: 2,
            duration_ms: None,
        });
    }

    // 4. Add expression from emotion
    if let Some(ref em) = emotion {
        commands.push(RobotCommand {
            cmd_type: "face".into(),
            params: serde_json::json!({"expression": em}),
            priority: 2,
            duration_ms: Some(3000),
        });
    }

    // Sort by priority
    commands.sort_by_key(|c| c.priority);

    let duration_ms = start.elapsed().as_millis() as u64;

    BehaviorDecision {
        commands,
        reasoning: None,
        should_speak,
        emotion,
        duration_ms,
    }
}

/// Map NLU intent to robot commands.
fn map_intent_to_commands(nlu: &NluResult) -> Vec<RobotCommand> {
    let mut cmds = Vec::new();

    match nlu.intent.as_str() {
        "move" => {
            if let Some(sub) = &nlu.sub_intent {
                match sub.as_str() {
                    "forward" => cmds.push(RobotCommand {
                        cmd_type: "move".into(),
                        params: serde_json::json!({"direction": "forward", "speed": 128, "duration_ms": 2000}),
                        priority: 1,
                        duration_ms: Some(2000),
                    }),
                    "backward" => cmds.push(RobotCommand {
                        cmd_type: "move".into(),
                        params: serde_json::json!({"direction": "backward", "speed": 100, "duration_ms": 1500}),
                        priority: 1,
                        duration_ms: Some(1500),
                    }),
                    "left" => cmds.push(RobotCommand {
                        cmd_type: "move".into(),
                        params: serde_json::json!({"direction": "rotate_left", "speed": 100, "duration_ms": 1000}),
                        priority: 1,
                        duration_ms: Some(1000),
                    }),
                    "right" => cmds.push(RobotCommand {
                        cmd_type: "move".into(),
                        params: serde_json::json!({"direction": "rotate_right", "speed": 100, "duration_ms": 1000}),
                        priority: 1,
                        duration_ms: Some(1000),
                    }),
                    "stop" => cmds.push(RobotCommand {
                        cmd_type: "stop".into(),
                        params: serde_json::json!({"type": "gradual"}),
                        priority: 0,
                        duration_ms: None,
                    }),
                    "follow" => cmds.push(RobotCommand {
                        cmd_type: "move".into(),
                        params: serde_json::json!({"direction": "forward", "speed": 80, "duration_ms": 0}),
                        priority: 1,
                        duration_ms: None,
                    }),
                    "dance" => {
                        cmds.push(RobotCommand {
                            cmd_type: "move".into(),
                            params: serde_json::json!({"direction": "rotate_left", "speed": 150, "duration_ms": 500}),
                            priority: 1,
                            duration_ms: Some(500),
                        });
                        cmds.push(RobotCommand {
                            cmd_type: "led".into(),
                            params: serde_json::json!({"mode": "rainbow", "speed": 200}),
                            priority: 2,
                            duration_ms: Some(3000),
                        });
                    }
                    _ => {}
                }
            }
        }
        "expression" => {
            if let Some(sub) = &nlu.sub_intent {
                cmds.push(RobotCommand {
                    cmd_type: "face".into(),
                    params: serde_json::json!({"expression": sub}),
                    priority: 2,
                    duration_ms: Some(5000),
                });
            }
        }
        "led" => {
            if let Some(sub) = &nlu.sub_intent {
                match sub.as_str() {
                    "on" => cmds.push(RobotCommand {
                        cmd_type: "led".into(),
                        params: serde_json::json!({"mode": "solid", "color": {"r": 255, "g": 255, "b": 255}}),
                        priority: 2,
                        duration_ms: None,
                    }),
                    "off" => cmds.push(RobotCommand {
                        cmd_type: "led".into(),
                        params: serde_json::json!({"mode": "off"}),
                        priority: 2,
                        duration_ms: None,
                    }),
                    color @ ("red" | "green" | "blue") => {
                        let rgb = match color {
                            "red" => (255, 0, 0),
                            "green" => (0, 255, 0),
                            "blue" => (0, 0, 255),
                            _ => unreachable!(),
                        };
                        cmds.push(RobotCommand {
                            cmd_type: "led".into(),
                            params: serde_json::json!({
                                "mode": "solid",
                                "color": {"r": rgb.0, "g": rgb.1, "b": rgb.2}
                            }),
                            priority: 2,
                            duration_ms: None,
                        });
                    }
                    _ => {}
                }
            }
        }
        "action" => {
            match nlu.sub_intent.as_deref() {
                Some("charge") => cmds.push(RobotCommand {
                    cmd_type: "move".into(),
                    params: serde_json::json!({"direction": "charge_dock"}),
                    priority: 1,
                    duration_ms: None,
                }),
                Some("sleep") => cmds.push(RobotCommand {
                    cmd_type: "led".into(),
                    params: serde_json::json!({"mode": "off"}),
                    priority: 2,
                    duration_ms: None,
                }),
                Some("wake") => {
                    cmds.push(RobotCommand {
                        cmd_type: "led".into(),
                        params: serde_json::json!({"mode": "breathing", "color": {"r": 100, "g": 200, "b": 255}}),
                        priority: 2,
                        duration_ms: Some(2000),
                    });
                    cmds.push(RobotCommand {
                        cmd_type: "face".into(),
                        params: serde_json::json!({"expression": "happy"}),
                        priority: 2,
                        duration_ms: Some(3000),
                    });
                }
                Some("photo") => cmds.push(RobotCommand {
                    cmd_type: "camera_capture".into(),
                    params: serde_json::json!({"format": "jpeg"}),
                    priority: 3,
                    duration_ms: None,
                }),
                _ => {}
            }
        }
        _ => {}
    }

    cmds
}
