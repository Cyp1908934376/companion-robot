/**
 * Behavior engine — high-level robot personality and autonomy.
 *
 * 50Hz state machine coordinating perception → action:
 *   IDLE → INTERACTING (on touch/voice) → IDLE (after 30s quiet)
 *   IDLE → EXPLORING (if autonomous mode)
 *   Any  → CHARGING (if low battery)
 *   Any  → ALERT (on anomaly detection)
 */

#include "behavior.h"
#include "offline.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include <stdlib.h>

/* Include component APIs for sensor fusion */
#include "imu.h"
#include "touch.h"
#include "face.h"
#include "led.h"
#include "servo.h"
#include "motor.h"
#include "power_mgr.h"
#include "obstacle.h"

static const char *TAG = "behavior";

/* ── External queues and events ───────────────────────────────── */

extern QueueHandle_t q_sensor_event;
extern QueueHandle_t q_cmd_outgoing;
extern EventGroupHandle_t evg_system;

/* ── Behavior state ───────────────────────────────────────────── */

static behavior_state_t g_state = BEHAVIOR_IDLE;
static behavior_state_t g_prev_state = BEHAVIOR_IDLE;
static uint32_t g_state_entered_ms;
static uint32_t g_last_interaction_ms;

/* Idle animation timers */
static uint32_t g_next_idle_action_ms;
#define IDLE_ACTION_MIN_MS  5000
#define IDLE_ACTION_MAX_MS  10000

/* Interaction timeout */
#define INTERACTION_TIMEOUT_MS  30000  /* return to IDLE after 30s quiet */

/* ── State change helper ──────────────────────────────────────── */

static void transition_to(behavior_state_t new_state) {
    if (new_state == g_state) return;

    ESP_LOGI(TAG, "behavior transition: %d → %d", g_state, new_state);
    g_prev_state = g_state;
    g_state = new_state;
    g_state_entered_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    /* State entry actions */
    switch (new_state) {
    case BEHAVIOR_IDLE:
        led_set_solid((led_color_t){0, 0, 20});  /* dim blue */
        face_set_expression(BCP_FACE_NEUTRAL);
        break;
    case BEHAVIOR_EXPLORING:
        led_set_solid((led_color_t){0, 20, 0});  /* dim green */
        face_set_expression(BCP_FACE_NEUTRAL);
        break;
    case BEHAVIOR_INTERACTING:
        led_set_solid((led_color_t){20, 20, 0});  /* warm yellow */
        face_set_expression(BCP_FACE_HAPPY);
        break;
    case BEHAVIOR_CHARGING:
        led_set_pattern((led_pattern_t){
            .mode = BCP_LED_BREATHING, .speed = 20, .color = {0, 0, 50}
        });
        face_set_expression(BCP_FACE_SLEEPY);
        break;
    case BEHAVIOR_PLAYING:
        led_set_pattern((led_pattern_t){
            .mode = BCP_LED_RAINBOW, .speed = 80, .color = {255, 255, 255}
        });
        face_set_expression(BCP_FACE_HAPPY);
        break;
    case BEHAVIOR_ALERT:
        led_set_pattern((led_pattern_t){
            .mode = BCP_LED_BLINKING, .speed = 200, .color = {255, 0, 0}
        });
        face_set_expression(BCP_FACE_SURPRISED);
        break;
    case BEHAVIOR_ERROR:
        led_set_pattern((led_pattern_t){
            .mode = BCP_LED_BLINKING, .speed = 100, .color = {255, 0, 0}
        });
        face_set_expression(BCP_FACE_ANGRY);
        break;
    }
}

/* ── Idle animations ──────────────────────────────────────────── */

static void idle_animation(void) {
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (now < g_next_idle_action_ms) return;

    /* Pick a random idle action */
    int action = rand() % 4;
    switch (action) {
    case 0:
        /* Slow head pan */
        servo_set_angle(SERVO_PAN, (rand() % 60) - 30, 30);
        break;
    case 1:
        /* Blink (via face subsystem) */
        face_blink_tick();
        break;
    case 2:
        /* Subtle LED pulse */
        led_set_pattern((led_pattern_t){
            .mode = BCP_LED_BREATHING, .speed = 10,
            .color = {0, 0, (uint8_t)(10 + rand() % 20)}
        });
        break;
    case 3:
        /* Nothing — just wait */
        break;
    }

    g_next_idle_action_ms = now + IDLE_ACTION_MIN_MS
                            + (rand() % (IDLE_ACTION_MAX_MS - IDLE_ACTION_MIN_MS));
}

/* ── Exploring behavior ───────────────────────────────────────── */

static void exploring_behavior(void) {
    /* Simple wall-following: check obstacle, turn away from nearest wall */
    obstacle_status_t obs = obstacle_check();

    if (obs.emergency_stop) {
        /* Back away */
        motor_cmd_t cmd = { .type = MOTOR_CMD_MOVE, .move.direction = BCP_DIR_BACKWARD, .move.speed = 60, .duration_ms = 500 };
        xQueueSend(q_cmd_incoming, &cmd, 0);
    } else if (obs.slow_down) {
        /* Turn away from obstacle */
        uint16_t min_dist = 0xFFFF;
        int min_dir = -1;
        for (int i = 0; i < OBS_DIR_COUNT; i++) {
            if (obs.distance_cm[i] > 0 && obs.distance_cm[i] < min_dist) {
                min_dist = obs.distance_cm[i];
                min_dir = i;
            }
        }
        if (min_dir == OBS_FRONT) {
            motor_cmd_t cmd = { .type = MOTOR_CMD_MOVE, .move.direction = BCP_DIR_ROTATE_LEFT, .move.speed = 50, .duration_ms = 300 };
            xQueueSend(q_cmd_incoming, &cmd, 0);
        } else if (min_dir == OBS_FRONT_LEFT) {
            motor_cmd_t cmd = { .type = MOTOR_CMD_MOVE, .move.direction = BCP_DIR_ROTATE_RIGHT, .move.speed = 50, .duration_ms = 200 };
            xQueueSend(q_cmd_incoming, &cmd, 0);
        } else if (min_dir == OBS_FRONT_RIGHT) {
            motor_cmd_t cmd = { .type = MOTOR_CMD_MOVE, .move.direction = BCP_DIR_ROTATE_LEFT, .move.speed = 50, .duration_ms = 200 };
            xQueueSend(q_cmd_incoming, &cmd, 0);
        }
    } else {
        /* Clear path — explore forward at low speed */
        motor_cmd_t cmd = { .type = MOTOR_CMD_MOVE, .move.direction = BCP_DIR_FORWARD, .move.speed = 30, .duration_ms = 0 };
        xQueueSend(q_cmd_incoming, &cmd, 0);
    }
}

/* ── Main behavior task ───────────────────────────────────────── */

void task_behavior(void *arg) {
    ESP_LOGI(TAG, "behavior engine task started (50Hz)");

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(20);  /* 50Hz */

    /* Seed random for idle animations */
    srand(esp_timer_get_time());

    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    g_state_entered_ms = now;
    g_last_interaction_ms = now;

    /* Start in IDLE */
    transition_to(BEHAVIOR_IDLE);

    while (1) {
        now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        /* ── Sensor fusion: check for events ──────────────────── */
        touch_event_t touch_evt;
        bool has_touch = (xQueueReceive(q_sensor_event, &touch_evt, 0) == pdTRUE);

        /* Check power state for forced transitions */
        power_state_t power = power_mgr_get_state();
        EventBits_t flags = xEventGroupGetBits(evg_system);

        /* ── State machine ────────────────────────────────────── */

        /* Global override: power critical → CHARGING (seek charger) */
        if (power == POWER_CRITICAL && g_state != BEHAVIOR_CHARGING) {
            transition_to(BEHAVIOR_CHARGING);
        }

        /* Global override: emergency flag → ALERT */
        if (flags & EVG_EMERGENCY && g_state != BEHAVIOR_ALERT
            && power != POWER_CRITICAL) {
            transition_to(BEHAVIOR_ALERT);
        }

        /* Per-state behavior */
        switch (g_state) {
        case BEHAVIOR_IDLE:
            /* Idle animations */
            idle_animation();

            /* Touch → INTERACTING */
            if (has_touch && touch_evt.touched) {
                g_last_interaction_ms = now;
                transition_to(BEHAVIOR_INTERACTING);
            }
            break;

        case BEHAVIOR_EXPLORING:
            exploring_behavior();

            /* Touch → INTERACTING */
            if (has_touch && touch_evt.touched) {
                g_last_interaction_ms = now;
                transition_to(BEHAVIOR_INTERACTING);
            }

            /* Critical battery → CHARGING */
            if (power == POWER_LOW_BATTERY) {
                transition_to(BEHAVIOR_CHARGING);
            }
            break;

        case BEHAVIOR_INTERACTING: {
            /* Respond to touch: turn toward touch zone */
            if (has_touch && touch_evt.touched) {
                g_last_interaction_ms = now;

                switch (touch_evt.zone) {
                case TOUCH_ZONE_HEAD:
                    /* Petting — happy expression, wag (rotate slightly) */
                    face_set_expression(BCP_FACE_HAPPY);
                    led_set_solid((led_color_t){20, 20, 0});
                    servo_set_angle(SERVO_TILT, -10, 30);  /* look up */
                    break;
                case TOUCH_ZONE_BACK:
                    /* Carrying — neutral, still */
                    face_set_expression(BCP_FACE_NEUTRAL);
                    break;
                case TOUCH_ZONE_LEFT:
                    /* Nudge left — turn left */
                    servo_set_angle(SERVO_PAN, -30, 45);
                    break;
                case TOUCH_ZONE_RIGHT:
                    /* Nudge right — turn right */
                    servo_set_angle(SERVO_PAN, 30, 45);
                    break;
                default:
                    break;
                }
            }

            /* Timeout: no interaction for 30s → IDLE */
            if (now - g_last_interaction_ms > INTERACTION_TIMEOUT_MS) {
                /* Return servos to center */
                servo_set_angle(SERVO_PAN, 0, 30);
                servo_set_angle(SERVO_TILT, 0, 30);
                transition_to(BEHAVIOR_IDLE);
            }
            break;
        }

        case BEHAVIOR_CHARGING:
            /* If docked, stay still. If not docked but low battery,
             * navigate to charger (IR beacon tracking, simplified). */
            if (power == POWER_CHARGED) {
                transition_to(BEHAVIOR_IDLE);
            }
            break;

        case BEHAVIOR_PLAYING:
            /* Playing state: scripted sequences from external commands */
            /* TODO: execute behavior sequence from queue */
            if (now - g_state_entered_ms > 10000) {  /* max 10s play */
                transition_to(BEHAVIOR_IDLE);
            }
            break;

        case BEHAVIOR_ALERT:
            /* Alert: monitor for resolution */
            if (!(flags & EVG_EMERGENCY) && power != POWER_CRITICAL) {
                /* Emergency cleared */
                transition_to(BEHAVIOR_IDLE);
            }
            break;

        case BEHAVIOR_ERROR:
            /* Error: attempt recovery after 5s */
            if (now - g_state_entered_ms > 5000) {
                transition_to(BEHAVIOR_IDLE);
            }
            break;
        }

        vTaskDelayUntil(&last_wake, period);
        esp_task_wdt_reset();
    }
}
