/**
 * Offline mode — local autonomy when disconnected from main brain.
 *
 * Capabilities without network:
 *   - Basic obstacle avoidance (ToF + IMU)
 *   - Pre-loaded voice commands (~50 keywords, local NLU)
 *   - Pre-loaded facial expressions + LED patterns
 *   - Touch gesture responses
 *   - Environment monitoring + logging (SPIFFS)
 *
 * Activates when conn_manager reports CONN_STATE_OFFLINE.
 */

#include "offline.h"
#include "config.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_task_wdt.h"
#include <string.h>
#include <stdio.h>

/* Component APIs */
#include "conn_manager.h"
#include "motor.h"
#include "servo.h"
#include "led.h"
#include "face.h"
#include "obstacle.h"

static const char *TAG = "offline";

/* ── State ────────────────────────────────────────────────────── */

static bool g_offline_active = false;
static uint32_t g_offline_started_ms;

extern EventGroupHandle_t evg_system;
extern QueueHandle_t q_cmd_incoming;

/* ── Local NLU keyword table ──────────────────────────────────── */

typedef struct {
    const char *keyword;
    uint16_t    cmd_id;
    uint16_t    arg1;       /* generic parameter */
    uint16_t    arg2;
} offline_keyword_t;

#define KW_COUNT 9

static const offline_keyword_t KEYWORDS[KW_COUNT] = {
    {"stop",        0x0001, BCP_DIR_STOP,          0},
    {"come here",   0x0001, BCP_DIR_FORWARD,       60},
    {"follow me",   0x0001, BCP_DIR_FORWARD,       60},
    {"dance",       0x0001, BCP_DIR_ROTATE_LEFT,    80},
    {"patrol",      0x0001, BCP_DIR_FORWARD,        30},
    {"go charge",   0x0001, BCP_DIR_FORWARD,        20},
    {"lights on",   0x0002, 255,                   255},  /* white */
    {"lights off",  0x0002, 0,                     0},
    {"status",      0x00FF, 0,                     0},     /* special: status report */
};

/* ── Local NLU ────────────────────────────────────────────────── */

static const offline_keyword_t *match_keyword(const char *text) {
    /* Simple substring match (case-insensitive) */
    for (int i = 0; i < KW_COUNT; i++) {
        const char *kw = KEYWORDS[i].keyword;
        const char *pos = text;
        while (*pos) {
            /* Case-insensitive compare */
            const char *a = kw;
            const char *b = pos;
            bool match = true;
            while (*a && *b) {
                char ca = (*a >= 'A' && *a <= 'Z') ? (*a + 32) : *a;
                char cb = (*b >= 'A' && *b <= 'Z') ? (*b + 32) : *b;
                if (ca != cb) { match = false; break; }
                a++; b++;
            }
            if (match && *a == '\0') {
                return &KEYWORDS[i];
            }
            pos++;
        }
    }
    return NULL;
}

static int execute_offline_command(const offline_keyword_t *kw) {
    if (!kw) return -1;

    switch (kw->cmd_id) {
    case 0x0001: {  /* movement command */
        motor_cmd_t cmd = {
            .type = MOTOR_CMD_MOVE,
            .move.direction   = (bcp_direction_t)kw->arg1,
            .move.speed       = (uint8_t)kw->arg2,
            .duration_ms = (kw->arg2 == 0) ? 0 : 2000,  /* 2s for directional commands */
            .no_ack      = true,
        };
        xQueueSend(q_cmd_incoming, &cmd, 0);
        ESP_LOGI(TAG, "offline motor: dir=%d speed=%d", cmd.move.direction, cmd.move.speed);
        break;
    }
    case 0x0002:  /* LED command */
        if (kw->arg1 == 0) {
            led_off();
        } else {
            led_set_solid((led_color_t){255, 255, 255});
        }
        break;
    case 0x00FF:  /* Status report */
        ESP_LOGI(TAG, "status: offline_active=%d", g_offline_active);
        led_set_pattern((led_pattern_t){
            .mode = BCP_LED_BLINKING, .speed = 100, .color = {0, 255, 0}
        });
        break;
    default:
        ESP_LOGD(TAG, "unknown offline command: 0x%04X", kw->cmd_id);
        return -1;
    }
    return 0;
}

/* ── SPIFFS logging ───────────────────────────────────────────── */

static void log_sensor_data(void) {
    /* Append sensor readings to a SPIFFS log file for later sync */
    FILE *f = fopen("/spiffs/offline_log.csv", "a");
    if (!f) return;

    /* Log timestamp and basic state */
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    /* Read latest sensor data via obstacle check */
    obstacle_status_t obs = obstacle_check();

    fprintf(f, "%lu,%d,%d,%d,%d,%d\n",
            now,
            obs.emergency_stop,
            obs.slow_down,
            obs.distance_cm[0],
            obs.distance_cm[1],
            obs.distance_cm[2]);

    fclose(f);
}

/* ── Public API ───────────────────────────────────────────────── */

void offline_init(void) {
    ESP_LOGI(TAG, "initializing offline mode subsystem");

    /* Initialize SPIFFS for offline data logging */
    esp_vfs_spiffs_conf_t spiffs_cfg = {
        .base_path              = "/spiffs",
        .partition_label        = NULL,
        .max_files              = 5,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&spiffs_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed: %d — logging disabled", err);
    } else {
        size_t total = 0, used = 0;
        esp_spiffs_info(NULL, &total, &used);
        ESP_LOGI(TAG, "SPIFFS ready: %d/%d KB used", used / 1024, total / 1024);
    }

    ESP_LOGI(TAG, "offline NLU loaded (%d keywords)", KW_COUNT);
}

bool offline_is_active(void) {
    return g_offline_active;
}

int offline_process_command(const bcp_command_t *cmd) {
    if (!cmd) return -1;

    /* In production: decode command and execute locally.
     * For now, handle basic movement and LED commands. */
    ESP_LOGD(TAG, "offline command: cmd_id=0x%04X", cmd->cmd_id);

    /* Match against keyword table by cmd_id */
    for (int i = 0; i < KW_COUNT; i++) {
        if (KEYWORDS[i].cmd_id == cmd->cmd_id) {
            return execute_offline_command(&KEYWORDS[i]);
        }
    }

    return 0;
}

/* ── Offline autonomy task ────────────────────────────────────── */

void task_offline(void *arg) {
    ESP_LOGI(TAG, "offline autonomy task started (20Hz)");

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(50);  /* 20Hz */

    uint32_t log_counter = 0;

    while (1) {
        /* Check connection state */
        conn_state_t conn = g_conn_mgr.state;

        if (conn == CONN_STATE_OFFLINE) {
            if (!g_offline_active) {
                /* Entering offline mode */
                g_offline_active = true;
                g_offline_started_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
                ESP_LOGW(TAG, "offline mode activated");

                /* Visual indicator */
                led_set_pattern((led_pattern_t){
                    .mode = BCP_LED_BLINKING, .speed = 50, .color = {255, 165, 0}  /* orange */
                });
                face_set_expression(BCP_FACE_SLEEPY);
            }

            /* ── Offline behavior ──────────────────────────── */

            /* 1. Obstacle avoidance (always active in offline) */
            obstacle_status_t obs = obstacle_check();
            if (obs.emergency_stop) {
                motor_cmd_t stop_cmd = {
                    .type = MOTOR_CMD_STOP,
                    .duration_ms = 0
                };
                xQueueSend(q_cmd_incoming, &stop_cmd, 0);
            }

            /* 2. Log sensor data periodically */
            if (++log_counter % 20 == 0) {  /* every 1s at 20Hz */
                log_sensor_data();
            }

            /* 3. Roam/explore (if not docked or critical) */
            if (!obs.emergency_stop) {
                /* Slow patrol */
                static int explore_counter = 0;
                if (++explore_counter > 100) {  /* change direction every 5s */
                    explore_counter = 0;
                    motor_cmd_t explore_cmd = {
                        .type = MOTOR_CMD_MOVE,
                        .move.direction   = (rand() % 2) ? BCP_DIR_ROTATE_LEFT : BCP_DIR_ROTATE_RIGHT,
                        .move.speed       = 30,
                        .duration_ms = 500,
                    };
                    xQueueSend(q_cmd_incoming, &explore_cmd, 0);
                } else {
                    motor_cmd_t fwd_cmd = {
                        .type = MOTOR_CMD_MOVE,
                        .move.direction = BCP_DIR_FORWARD,
                        .move.speed = 20,
                        .duration_ms = 0
                    };
                    xQueueSend(q_cmd_incoming, &fwd_cmd, 0);
                }
            }

        } else if (g_offline_active) {
            /* Connection recovered — exit offline mode */
            g_offline_active = false;
            uint32_t duration = (xTaskGetTickCount() * portTICK_PERIOD_MS) - g_offline_started_ms;
            ESP_LOGI(TAG, "offline mode deactivated (duration=%lu ms)", duration);

            /* Restore normal LED state */
            led_set_solid((led_color_t){0, 0, 20});
            face_set_expression(BCP_FACE_NEUTRAL);

            /* Stop any ongoing movement */
            motor_cmd_t stop_cmd = {
                .type = MOTOR_CMD_STOP,
                .duration_ms = 0
            };
            xQueueSend(q_cmd_incoming, &stop_cmd, 0);
        }

        vTaskDelayUntil(&last_wake, period);
        esp_task_wdt_reset();
    }
}
