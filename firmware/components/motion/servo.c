/**
 * Servo driver — head pan/tilt SG90.
 *
 * LEDC PWM at 50Hz (20ms period), 0-180° mapped to 500-2500us duty.
 * Software ramp with acceleration profile for smooth motion.
 */

#include "servo.h"
#include "config.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_task_wdt.h"

static const char *TAG = "servo";

/* ── Constants ────────────────────────────────────────────────── */

#define SERVO_PWM_FREQ      50
#define SERVO_TIMER         LEDC_TIMER_1
#define SERVO_DUTY_RES      LEDC_TIMER_14_BIT
#define SERVO_MAX_DUTY      16383  /* 14-bit max */

/* Pulse widths (us) at 50Hz: 20ms period = 20000us */
#define PULSE_MIN_US        500
#define PULSE_MAX_US        2500
#define PULSE_CENTER_US     1500
#define PERIOD_US           20000

/* Angle limits */
#define PAN_MIN_DEG         -90
#define PAN_MAX_DEG          90
#define TILT_MIN_DEG        -45
#define TILT_MAX_DEG         45

/* ── Servo state ──────────────────────────────────────────────── */

typedef struct {
    servo_id_t id;
    ledc_channel_t channel;
    gpio_num_t gpio;
    int16_t min_deg;
    int16_t max_deg;
} servo_config_t;

static const servo_config_t SERVO_CFG[SERVO_COUNT] = {
    [SERVO_PAN]  = { .id = SERVO_PAN,  .channel = LEDC_CHANNEL_0,
                     .gpio = SERVO_PAN_IO,  .min_deg = PAN_MIN_DEG,  .max_deg = PAN_MAX_DEG },
    [SERVO_TILT] = { .id = SERVO_TILT, .channel = LEDC_CHANNEL_1,
                     .gpio = SERVO_TILT_IO, .min_deg = TILT_MIN_DEG, .max_deg = TILT_MAX_DEG },
};

typedef struct {
    int16_t  current_angle;   /* current actual angle */
    int16_t  target_angle;    /* desired angle */
    uint16_t speed_dps;       /* ramp speed (0 = instant) */
    bool     active;
} servo_state_t;

static servo_state_t g_servo[SERVO_COUNT];

/* External queue */
extern QueueHandle_t q_sensor_event;  /* reused for servo commands? No, use direct API */

/* ── Helpers ──────────────────────────────────────────────────── */

static int16_t clamp_angle(servo_id_t id, int16_t angle) {
    if (angle > SERVO_CFG[id].max_deg) return SERVO_CFG[id].max_deg;
    if (angle < SERVO_CFG[id].min_deg) return SERVO_CFG[id].min_deg;
    return angle;
}

/**
 * Convert angle (0-180°) to LEDC duty for 50Hz PWM.
 * 0° = 500us, 180° = 2500us. Center (90°) = 1500us.
 */
static uint32_t angle_to_duty(int16_t angle_deg) {
    /* Map from [-90, 90] to [0, 180] then to duty */
    int16_t normalized = angle_deg + 90;  /* 0..180 */
    uint32_t pulse_us = PULSE_MIN_US +
        (uint32_t)((int32_t)(PULSE_MAX_US - PULSE_MIN_US) * normalized / 180);
    uint32_t duty = (uint32_t)((uint64_t)pulse_us * SERVO_MAX_DUTY / PERIOD_US);
    return duty;
}

static void set_pwm(servo_id_t id, int16_t angle) {
    uint32_t duty = angle_to_duty(clamp_angle(id, angle));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, SERVO_CFG[id].channel, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, SERVO_CFG[id].channel);
}

/* ── Public API ───────────────────────────────────────────────── */

void servo_init(void) {
    ESP_LOGI(TAG, "initializing servo PWM (pan/tilt)");

    /* Configure LEDC timer at 50Hz, 14-bit resolution */
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = SERVO_DUTY_RES,
        .timer_num       = SERVO_TIMER,
        .freq_hz         = SERVO_PWM_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    /* Configure LEDC channels */
    for (int i = 0; i < SERVO_COUNT; i++) {
        ledc_channel_config_t ch_cfg = {
            .gpio_num   = SERVO_CFG[i].gpio,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = SERVO_CFG[i].channel,
            .timer_sel  = SERVO_TIMER,
            .duty       = angle_to_duty(0),  /* center */
            .hpoint     = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

        /* Initialize state — both servos start centered (0°) */
        g_servo[i].current_angle = 0;
        g_servo[i].target_angle  = 0;
        g_servo[i].speed_dps     = 0;
        g_servo[i].active        = false;
    }

    ESP_LOGI(TAG, "servo driver ready (pan=%d..%d, tilt=%d..%d deg)",
             PAN_MIN_DEG, PAN_MAX_DEG, TILT_MIN_DEG, TILT_MAX_DEG);
}

void servo_set_angle(servo_id_t id, int16_t angle_deg, uint16_t speed_dps) {
    if (id >= SERVO_COUNT) return;

    int16_t clamped = clamp_angle(id, angle_deg);

    if (speed_dps == 0) {
        /* Instant move */
        g_servo[id].current_angle = clamped;
        g_servo[id].target_angle  = clamped;
        g_servo[id].active        = false;
        set_pwm(id, clamped);
    } else {
        /* Start ramped move — will be executed by task_servo_ctrl */
        g_servo[id].target_angle = clamped;
        g_servo[id].speed_dps    = speed_dps;
        g_servo[id].active       = true;
    }

    ESP_LOGD(TAG, "set servo %d: %d deg @ %d dps", id, angle_deg, speed_dps);
}

/* ── Servo control task ───────────────────────────────────────── */

void task_servo_ctrl(void *arg) {
    ESP_LOGI(TAG, "servo control task started (50Hz)");

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(20);  /* 50Hz */
    const float dt = 0.02f;  /* 20ms */

    while (1) {
        /* Process ramped moves */
        for (int i = 0; i < SERVO_COUNT; i++) {
            if (!g_servo[i].active) continue;

            int16_t current = g_servo[i].current_angle;
            int16_t target  = g_servo[i].target_angle;
            int16_t max_step = (int16_t)(g_servo[i].speed_dps * dt);

            if (max_step < 1) max_step = 1;

            if (current < target) {
                current += max_step;
                if (current > target) current = target;
            } else if (current > target) {
                current -= max_step;
                if (current < target) current = target;
            }

            g_servo[i].current_angle = current;
            set_pwm((servo_id_t)i, current);

            if (current == target) {
                g_servo[i].active = false;
            }
        }

        vTaskDelayUntil(&last_wake, period);
        esp_task_wdt_reset();
    }
}
