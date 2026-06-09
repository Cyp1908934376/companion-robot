/**
 * Motor driver — DRV8833 x2, 4x N20 micrometal gearmotors.
 *
 * Mecanum wheel kinematics + PID control at 100Hz.
 * 8 LEDC PWM channels (2 per motor), 20kHz, 10-bit resolution.
 */

#include "motor.h"
#include "obstacle.h"
#include "localization.h"
#include "config.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_task_wdt.h"
#include <math.h>

static const char *TAG = "motor";

/* ── PWM pin mapping ─────────────────────────────────────────── */

typedef struct {
    gpio_num_t a;  /* IN1 — forward */
    gpio_num_t b;  /* IN2 — reverse */
} motor_pins_t;

static const motor_pins_t MOTOR_PINS[MOTOR_COUNT] = {
    [MOTOR_L1] = { .a = MOTOR_L1_A_IO, .b = MOTOR_L1_B_IO },
    [MOTOR_L2] = { .a = MOTOR_L2_A_IO, .b = MOTOR_L2_B_IO },
    [MOTOR_R1] = { .a = MOTOR_R1_A_IO, .b = MOTOR_R1_B_IO },
    [MOTOR_R2] = { .a = MOTOR_R2_A_IO, .b = MOTOR_R2_B_IO },
};

/* ── LEDC handles ─────────────────────────────────────────────── */

static ledc_channel_t ledc_ch_a[MOTOR_COUNT];
static ledc_channel_t ledc_ch_b[MOTOR_COUNT];

/* ── PID state ────────────────────────────────────────────────── */

#define PID_KP  0.5f
#define PID_KI  0.1f
#define PID_KD  0.05f

typedef struct {
    float integral;
    float prev_error;
    float setpoint;   /* target speed 0.0–1.0 */
} pid_state_t;

static pid_state_t g_pid[MOTOR_COUNT];

/* ── Motor state ──────────────────────────────────────────────── */

static motor_cmd_t  g_current_cmd;
static bool         g_cmd_active;
static TickType_t   g_cmd_end_tick;
static bool         g_emergency_stop;

/* ── Move-to state (position-based navigation) ───────────────── */
static bool     g_move_to_active;
static int16_t  g_move_to_target_x;
static int16_t  g_move_to_target_y;
static uint8_t  g_move_to_speed;
static float    g_move_to_integral;    /* PID integral for distance control */
static float    g_move_to_prev_error;  /* PID derivative memory */

#define MOVE_TO_DIST_KP   0.4f
#define MOVE_TO_DIST_KI   0.02f
#define MOVE_TO_DIST_KD   0.1f
#define MOVE_TO_ARRIVE_CM 3.0f    /* within 3cm = arrived */
#define MOVE_TO_MAX_SPEED 0.8f    /* max duty for position control */

/* External queues (defined in main.c) */
extern QueueHandle_t q_cmd_incoming;

/* ── Helpers ──────────────────────────────────────────────────── */

static inline float clamp_f(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void set_motor_pwm(motor_id_t id, float duty) {
    /* duty: -1.0 (full reverse) .. 1.0 (full forward), 0 = brake/coast */
    uint32_t pwm_val;
    if (duty > 0.01f) {
        /* Forward: IN1 = PWM, IN2 = LOW */
        pwm_val = (uint32_t)(duty * 1023.0f);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, ledc_ch_a[id], pwm_val);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ledc_ch_a[id]);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, ledc_ch_b[id], 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ledc_ch_b[id]);
    } else if (duty < -0.01f) {
        /* Reverse: IN1 = LOW, IN2 = PWM */
        pwm_val = (uint32_t)(-duty * 1023.0f);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, ledc_ch_a[id], 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ledc_ch_a[id]);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, ledc_ch_b[id], pwm_val);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ledc_ch_b[id]);
    } else {
        /* Coast: both LOW */
        ledc_set_duty(LEDC_LOW_SPEED_MODE, ledc_ch_a[id], 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ledc_ch_a[id]);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, ledc_ch_b[id], 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ledc_ch_b[id]);
    }
}

static void brake_all(void) {
    for (int i = 0; i < MOTOR_COUNT; i++) {
        /* Brake: both IN1 and IN2 HIGH shorts the motor */
        ledc_set_duty(LEDC_LOW_SPEED_MODE, ledc_ch_a[i], 1023);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ledc_ch_a[i]);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, ledc_ch_b[i], 1023);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ledc_ch_b[i]);
        g_pid[i].integral = 0.0f;
        g_pid[i].prev_error = 0.0f;
        g_pid[i].setpoint = 0.0f;
    }
}

/* ── Mecanum kinematics ───────────────────────────────────────── */

/**
 * Compute per-wheel speeds from desired direction and speed.
 * Robot coordinate frame: +X = forward, +Y = left, +rotation = CCW
 *
 * Returns wheel duties in [-1.0, 1.0] for MOTOR_L1..MOTOR_R2.
 */
static void mecanum_kinematics(bcp_direction_t dir, float speed,
                               float *l1, float *l2, float *r1, float *r2) {
    float vx = 0.0f, vy = 0.0f, omega = 0.0f;

    switch (dir) {
    case BCP_DIR_FORWARD:        vx = speed;                  break;
    case BCP_DIR_BACKWARD:       vx = -speed;                  break;
    case BCP_DIR_LEFT:           vy = speed;                  break;
    case BCP_DIR_RIGHT:          vy = -speed;                  break;
    case BCP_DIR_FORWARD_LEFT:   vx = speed;   vy = speed;    break;
    case BCP_DIR_FORWARD_RIGHT:  vx = speed;   vy = -speed;   break;
    case BCP_DIR_BACKWARD_LEFT:  vx = -speed;  vy = speed;    break;
    case BCP_DIR_BACKWARD_RIGHT: vx = -speed;  vy = -speed;   break;
    case BCP_DIR_ROTATE_LEFT:    omega = speed;               break;
    case BCP_DIR_ROTATE_RIGHT:   omega = -speed;              break;
    case BCP_DIR_STOP:
    default:                                                  break;
    }

    /* Standard mecanum formula:
     *   Wheel = vx +/- vy +/- omega * (lx + ly)
     *   L1 = vx - vy - omega*(lx+ly)
     *   L2 = vx + vy - omega*(lx+ly)
     *   R1 = vx + vy + omega*(lx+ly)
     *   R2 = vx - vy + omega*(lx+ly)
     * With approximate wheelbase factor = 1.0 for small robot
     */
    const float wb = 1.0f;
    *l1 = vx - vy - omega * wb;
    *l2 = vx + vy - omega * wb;
    *r1 = vx + vy + omega * wb;
    *r2 = vx - vy + omega * wb;

    /* Normalize if any wheel exceeds 1.0 */
    float max_val = fmaxf(fmaxf(fabsf(*l1), fabsf(*l2)), fmaxf(fabsf(*r1), fabsf(*r2)));
    if (max_val > 1.0f) {
        *l1 /= max_val;
        *l2 /= max_val;
        *r1 /= max_val;
        *r2 /= max_val;
    }
}

/* ── PID controller ───────────────────────────────────────────── */

static float pid_update(pid_state_t *pid, float measured, float dt) {
    float error = pid->setpoint - measured;
    pid->integral += error * dt;
    pid->integral = clamp_f(pid->integral, -1.0f, 1.0f);  /* anti-windup */
    float derivative = (error - pid->prev_error) / dt;
    pid->prev_error = error;
    return PID_KP * error + PID_KI * pid->integral + PID_KD * derivative;
}

/* ── Public API ───────────────────────────────────────────────── */

void motor_init(void) {
    ESP_LOGI(TAG, "initializing DRV8833 motor drivers");

    /* Configure LEDC timer (20kHz, 10-bit) */
    ledc_timer_config_t timer_cfg = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .duty_resolution  = MOTOR_PWM_RESOLUTION,
        .timer_num        = LEDC_TIMER_0,
        .freq_hz          = MOTOR_PWM_FREQ_HZ,
        .clk_cfg          = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    /* Configure 8 LEDC channels (2 per motor) */
    ledc_channel_t ch_list[MOTOR_COUNT * 2];
    for (int i = 0; i < MOTOR_COUNT; i++) {
        ch_list[i * 2]     = (ledc_channel_t)(LEDC_CHANNEL_0 + i * 2);
        ch_list[i * 2 + 1] = (ledc_channel_t)(LEDC_CHANNEL_0 + i * 2 + 1);
    }

    for (int i = 0; i < MOTOR_COUNT; i++) {
        ledc_ch_a[i] = ch_list[i * 2];
        ledc_ch_b[i] = ch_list[i * 2 + 1];

        ledc_channel_config_t ch_cfg_a = {
            .gpio_num   = MOTOR_PINS[i].a,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = ledc_ch_a[i],
            .timer_sel  = LEDC_TIMER_0,
            .duty       = 0,
            .hpoint     = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg_a));

        ledc_channel_config_t ch_cfg_b = {
            .gpio_num   = MOTOR_PINS[i].b,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = ledc_ch_b[i],
            .timer_sel  = LEDC_TIMER_0,
            .duty       = 0,
            .hpoint     = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg_b));
    }

    /* Initialize PID state */
    for (int i = 0; i < MOTOR_COUNT; i++) {
        g_pid[i] = (pid_state_t){0};
    }

    g_cmd_active = false;
    g_emergency_stop = false;

    /* Load calibration offsets from NVS if available */
    /* TODO: nvs_config_get_motor_cal() — Phase 3 calibration */

    ESP_LOGI(TAG, "motor driver ready (PWM %dHz, %d-bit)",
             MOTOR_PWM_FREQ_HZ, MOTOR_PWM_RESOLUTION);
}

/* ── Move-to API ───────────────────────────────────────────────── */

void motor_move_to(int16_t x_cm, int16_t y_cm, uint8_t speed) {
    g_move_to_target_x = x_cm;
    g_move_to_target_y = y_cm;
    g_move_to_speed = speed;
    g_move_to_integral = 0.0f;
    g_move_to_prev_error = 0.0f;
    g_move_to_active = true;
    g_cmd_active = false;  /* cancel any direction-based command */
    ESP_LOGI(TAG, "move_to: target=(%d,%d) speed=%d", x_cm, y_cm, speed);
}

void motor_cancel_move_to(void) {
    if (g_move_to_active) {
        g_move_to_active = false;
        brake_all();
        ESP_LOGI(TAG, "move_to cancelled");
    }
}

bool motor_move_to_active(void) {
    return g_move_to_active;
}

/* ── Motor control task ───────────────────────────────────────── */

void task_motor_ctrl(void *arg) {
    ESP_LOGI(TAG, "motor control task started (100Hz)");

    motor_cmd_t cmd;
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(10);  /* 100Hz */
    const float dt = 0.01f;  /* 100Hz = 10ms */

    while (1) {
        /* Receive command if available (non-blocking) */
        if (xQueueReceive(q_cmd_incoming, &cmd, 0) == pdTRUE) {
            if (cmd.type == MOTOR_CMD_STOP) {
                g_cmd_active = false;
                g_move_to_active = false;
                brake_all();
                ESP_LOGD(TAG, "stop command");
            } else if (cmd.type == MOTOR_CMD_MOVE_TO) {
                motor_move_to(cmd.move_to.x_cm, cmd.move_to.y_cm, cmd.move_to.speed);
            } else {
                g_current_cmd = cmd;
                g_cmd_active = true;
                g_move_to_active = false;  /* cancel move-to on new move cmd */
                if (cmd.duration_ms > 0) {
                    g_cmd_end_tick = xTaskGetTickCount()
                        + pdMS_TO_TICKS(cmd.duration_ms);
                } else {
                    g_cmd_end_tick = 0;  /* continuous */
                }
                ESP_LOGD(TAG, "cmd: dir=%d speed=%d dur=%lu",
                         cmd.move.direction, cmd.move.speed, cmd.duration_ms);
            }
        }

        /* Check obstacle status */
        obstacle_status_t obs = obstacle_check();

        /* Emergency stop check */
        if (obs.emergency_stop && !g_emergency_stop) {
            ESP_LOGW(TAG, "emergency stop triggered");
            g_emergency_stop = true;
            g_move_to_active = false;
            brake_all();
            g_cmd_active = false;
        }
        if (!obs.emergency_stop && g_emergency_stop) {
            ESP_LOGI(TAG, "emergency cleared");
            g_emergency_stop = false;
        }

        if (g_emergency_stop) {
            vTaskDelayUntil(&last_wake, period);
            esp_task_wdt_reset();
            continue;
        }

        /* Check duration expiry */
        if (g_cmd_active && g_cmd_end_tick > 0) {
            if (xTaskGetTickCount() >= g_cmd_end_tick) {
                g_cmd_active = false;
                for (int i = 0; i < MOTOR_COUNT; i++) {
                    g_pid[i].setpoint = 0.0f;
                }
                ESP_LOGD(TAG, "duration expired — stopping");
            }
        }

        /* Compute target speeds */
        float target[MOTOR_COUNT] = {0};

        if (g_move_to_active) {
            /* ── Position-based navigation using localization ── */
            pose_t pose;
            if (localization_get_pose(&pose)) {
                float dx = (float)g_move_to_target_x - pose.x_cm;
                float dy = (float)g_move_to_target_y - pose.y_cm;
                float dist = sqrtf(dx * dx + dy * dy);

                if (dist < MOVE_TO_ARRIVE_CM) {
                    /* Arrived at target */
                    g_move_to_active = false;
                    brake_all();
                    ESP_LOGI(TAG, "move_to arrived: (%.1f,%.1f)", pose.x_cm, pose.y_cm);
                } else {
                    /* Compute desired heading toward target */
                    float target_yaw = atan2f(dy, dx) * 180.0f / 3.14159265f;
                    float yaw_error = target_yaw - pose.yaw_deg;

                    /* Wrap yaw error to [-180, 180] */
                    while (yaw_error > 180.0f)  yaw_error -= 360.0f;
                    while (yaw_error < -180.0f) yaw_error += 360.0f;

                    /* PID for distance */
                    float speed_norm = clamp_f(g_move_to_speed / 255.0f, 0.0f, 1.0f);
                    g_move_to_integral += dist * dt;
                    g_move_to_integral = clamp_f(g_move_to_integral, -2.0f, 2.0f);
                    float d_deriv = (dist - g_move_to_prev_error) / dt;
                    g_move_to_prev_error = dist;

                    float dist_pid = MOVE_TO_DIST_KP * dist
                                   + MOVE_TO_DIST_KI * g_move_to_integral
                                   + MOVE_TO_DIST_KD * d_deriv;
                    dist_pid = clamp_f(dist_pid, 0.0f, MOVE_TO_MAX_SPEED);

                    /* Forward speed proportional to distance, scaled by yaw alignment */
                    float yaw_align = 1.0f - fminf(fabsf(yaw_error) / 90.0f, 1.0f);
                    float vx = dist_pid * yaw_align * speed_norm;

                    /* Rotation to face target */
                    float omega = clamp_f(yaw_error / 90.0f, -1.0f, 1.0f) * speed_norm;

                    /* Apply slowdown from obstacle detection */
                    if (obs.slow_down) {
                        vx *= 0.5f;
                        omega *= 0.5f;
                    }

                    /* Mecanum: forward + rotate */
                    const float wb = 1.0f;
                    target[MOTOR_L1] = vx - omega * wb;
                    target[MOTOR_L2] = vx - omega * wb;
                    target[MOTOR_R1] = vx + omega * wb;
                    target[MOTOR_R2] = vx + omega * wb;

                    /* Normalize */
                    float max_val = fmaxf(fmaxf(fabsf(target[MOTOR_L1]), fabsf(target[MOTOR_L2])),
                                         fmaxf(fabsf(target[MOTOR_R1]), fabsf(target[MOTOR_R2])));
                    if (max_val > 1.0f) {
                        for (int i = 0; i < MOTOR_COUNT; i++) target[i] /= max_val;
                    }
                }
            }
        } else if (g_cmd_active && g_current_cmd.move.direction != BCP_DIR_STOP) {
            float speed_norm = clamp_f(g_current_cmd.move.speed / 255.0f, 0.0f, 1.0f);

            /* Apply slow-down from obstacle detection */
            if (obs.slow_down) {
                speed_norm *= 0.5f;
                ESP_LOGD(TAG, "slow zone — speed reduced to %.0f%%", speed_norm * 100.0f);
            }

            mecanum_kinematics(g_current_cmd.move.direction, speed_norm,
                               &target[MOTOR_L1], &target[MOTOR_L2],
                               &target[MOTOR_R1], &target[MOTOR_R2]);
        }

        /* PID loop for each wheel */
        for (int i = 0; i < MOTOR_COUNT; i++) {
            g_pid[i].setpoint = target[i];
            float measured = (g_pid[i].setpoint > 0.01f || g_pid[i].setpoint < -0.01f)
                             ? g_pid[i].setpoint * 0.9f
                             : 0.0f;
            float output = pid_update(&g_pid[i], measured, dt);
            set_motor_pwm((motor_id_t)i, output);
        }

        vTaskDelayUntil(&last_wake, period);
        esp_task_wdt_reset();
    }
}
