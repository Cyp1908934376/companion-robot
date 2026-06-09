/**
 * Localization — Dead Reckoning + ToF Correction.
 *
 * EKF-based 6-DOF pose estimator fusing:
 *   - Motor odometry (open-loop commanded velocity from mecanum kinematics)
 *   - IMU yaw rate (gyro integration with zero-rotation detection)
 *   - ToF wall-distance correction (when walls are in view)
 *
 * State: [x_cm, y_cm, yaw_deg, vx_cm_s, vy_cm_s, vyaw_deg_s]
 */

#include "localization.h"
#include "config.h"
#include "bcp_codec.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_task_wdt.h"
#include <math.h>
#include <string.h>

static const char *TAG = "localization";

/* ── Global state ────────────────────────────────────────────── */

static localization_t g_loc;
static SemaphoreHandle_t g_loc_mutex;

/* External queues (defined in main.c) */
extern QueueHandle_t q_cmd_incoming;

/* ── Math helpers ────────────────────────────────────────────── */

static inline float deg_to_rad(float deg) { return deg * 3.14159265f / 180.0f; }
static inline float rad_to_deg(float rad) { return rad * 180.0f / 3.14159265f; }
static inline float sq(float x) { return x * x; }

/* ── Odometry model ──────────────────────────────────────────── */

/**
 * Convert a motor command (direction + speed) to robot-frame velocity.
 * Uses mecanum kinematics to compute vx, vy, omega from commanded wheel speeds.
 *
 * @param cmd      motor command
 * @param vx_out   forward velocity (cm/s)
 * @param vy_out   lateral velocity (cm/s)
 * @param vyaw_out angular velocity (deg/s)
 */
static void command_to_velocity(const motor_cmd_t *cmd,
                                float *vx_out, float *vy_out, float *vyaw_out) {
    if (!cmd || cmd->type != MOTOR_CMD_MOVE || cmd->move.direction == BCP_DIR_STOP) {
        *vx_out = 0.0f;
        *vy_out = 0.0f;
        *vyaw_out = 0.0f;
        return;
    }

    float speed = (float)cmd->move.speed / 255.0f * LOC_MAX_SPEED_CM_S;

    float vx = 0.0f, vy = 0.0f, omega = 0.0f;
    switch (cmd->move.direction) {
    case BCP_DIR_FORWARD:        vx = speed;                    break;
    case BCP_DIR_BACKWARD:       vx = -speed;                   break;
    case BCP_DIR_LEFT:           vy = speed;                   break;
    case BCP_DIR_RIGHT:          vy = -speed;                   break;
    case BCP_DIR_FORWARD_LEFT:   vx = speed;   vy = speed;     break;
    case BCP_DIR_FORWARD_RIGHT:  vx = speed;   vy = -speed;    break;
    case BCP_DIR_BACKWARD_LEFT:  vx = -speed;  vy = speed;     break;
    case BCP_DIR_BACKWARD_RIGHT: vx = -speed;  vy = -speed;    break;
    case BCP_DIR_ROTATE_LEFT:    omega = speed / LOC_WHEELBASE_CM; break;
    case BCP_DIR_ROTATE_RIGHT:   omega = -speed / LOC_WHEELBASE_CM; break;
    default: break;
    }

    *vx_out = vx;
    *vy_out = vy;
    *vyaw_out = rad_to_deg(omega);
}

/* ── EKF predict step ────────────────────────────────────────── */

/**
 * Predict state forward by dt seconds using the commanded velocity.
 *
 * State transition (constant velocity model with odometry input):
 *   x     += vx * cos(yaw) * dt - vy * sin(yaw) * dt
 *   y     += vx * sin(yaw) * dt + vy * cos(yaw) * dt
 *   yaw   += vyaw * dt
 *   vx    =  odom_vx   (measurement update from motor command)
 *   vy    =  odom_vy
 *   vyaw  =  odom_vyaw
 */
static void ekf_predict(float dt_s, float odom_vx, float odom_vy,
                        float odom_vyaw, float imu_yaw_rate) {
    pose_t *s = &g_loc.pose;
    float yaw_rad = deg_to_rad(s->yaw_deg);

    /* Integrate position using current velocity estimate */
    s->x_cm += s->vx_cm_s * cosf(yaw_rad) * dt_s
             - s->vy_cm_s * sinf(yaw_rad) * dt_s;
    s->y_cm += s->vx_cm_s * sinf(yaw_rad) * dt_s
             + s->vy_cm_s * cosf(yaw_rad) * dt_s;

    /* Integrate yaw using IMU gyro (more accurate than odometry omega) */
    s->yaw_deg += imu_yaw_rate * dt_s;

    /* Wrap yaw to [-180, 180] */
    while (s->yaw_deg > 180.0f)  s->yaw_deg -= 360.0f;
    while (s->yaw_deg < -180.0f) s->yaw_deg += 360.0f;

    /* Velocity: blend odometry with previous estimate */
    float alpha = 0.7f;  /* trust odometry more than prior */
    s->vx_cm_s   = alpha * odom_vx   + (1.0f - alpha) * s->vx_cm_s;
    s->vy_cm_s   = alpha * odom_vy   + (1.0f - alpha) * s->vy_cm_s;
    s->vyaw_deg_s = alpha * odom_vyaw + (1.0f - alpha) * s->vyaw_deg_s;

    /* Inflate covariance (process noise) */
    for (int i = 0; i < 6; i++) {
        float q = (i < 3) ? LOC_Q_POS : LOC_Q_VEL;
        g_loc.pose_covariance[i].x_cm += sq(q * dt_s);
    }
    g_loc.pose_covariance[2].x_cm += sq(LOC_Q_YAW * dt_s);
}

/* ── Zero-velocity detection ─────────────────────────────────── */

/**
 * Detect if the robot is stationary based on IMU and odometry.
 * When stopped, apply zero-velocity update (ZVU) to bound velocity drift.
 */
static void detect_zero_velocity(float odom_vx, float odom_vy,
                                  float odom_vyaw, float imu_accel_mag) {
    bool stopped = (fabsf(odom_vx) < LOC_ZERO_VEL_THRESH * LOC_MAX_SPEED_CM_S)
                && (fabsf(odom_vy) < LOC_ZERO_VEL_THRESH * LOC_MAX_SPEED_CM_S)
                && (fabsf(odom_vyaw) < 0.5f)
                && (imu_accel_mag < 0.15f * 9.81f);  /* < 0.15g */

    if (stopped) {
        if (!g_loc.zero_velocity) {
            g_loc.zero_velocity = true;
            g_loc.zero_velocity_duration_ms = 0;
        }
        /* Apply ZVU: pull velocity estimates toward zero */
        float zv_beta = 0.3f;  /* correction strength */
        g_loc.pose.vx_cm_s   *= (1.0f - zv_beta);
        g_loc.pose.vy_cm_s   *= (1.0f - zv_beta);
        g_loc.pose.vyaw_deg_s *= (1.0f - zv_beta);
    } else {
        g_loc.zero_velocity = false;
        g_loc.zero_velocity_duration_ms = 0;
    }
}

/* ── ToF correction ──────────────────────────────────────────── */

/**
 * Correct forward position using ToF wall distance.
 *
 * When the robot faces a wall, the ToF reading tells us the distance
 * to that wall. If we have a prior map or assume walls are orthogonal,
 * we can correct our x/y position.
 *
 * Simplified model: ToF forward = distance to nearest obstacle in
 * front hemisphere. Used to correct x_cm when moving forward.
 */
static void apply_tof_correction(void) {
    tof_reading_t tof;
    if (!tof_get_latest(&tof)) return;

    uint16_t front_dist = tof.distance_mm[0] / 10;  /* mm → cm */
    if (front_dist < 5 || front_dist > LOC_TOF_MAX_DIST_CM) return;

    /* Only correct when facing roughly forward (|yaw| < 30 deg) */
    float abs_yaw = fabsf(g_loc.pose.yaw_deg);
    if (abs_yaw > 30.0f) return;

    /* Correction: ToF tells us distance to wall in front.
     * Without a map, we can't do absolute correction. Instead,
     * use the rate-of-change of ToF to validate odometry.
     *
     * If ToF is decreasing at ~vx rate, odometry is plausible.
     * If ToF is changing differently, apply a gentle correction.
     */
    static float last_tof_dist = 0.0f;
    static uint32_t last_tof_ms = 0;

    if (last_tof_ms > 0) {
        float dt_s = (tof.timestamp_ms - last_tof_ms) / 1000.0f;
        if (dt_s > 0.05f && dt_s < 2.0f) {
            float tof_rate = (last_tof_dist - front_dist) / dt_s;  /* + = closing */
            float vel_diff = tof_rate - g_loc.pose.vx_cm_s;

            /* If velocity disagrees with ToF rate, nudge position */
            if (fabsf(vel_diff) > 10.0f) {  /* > 10 cm/s discrepancy */
                float correction = vel_diff * dt_s * LOC_TOF_CORRECT_GAIN;
                g_loc.pose.x_cm += correction;
            }
        }
    }

    last_tof_dist = (float)front_dist;
    last_tof_ms = tof.timestamp_ms;
    g_loc.last_tof_correct_ms = tof.timestamp_ms;
}

/* ── Public API ──────────────────────────────────────────────── */

void localization_init(void) {
    ESP_LOGI(TAG, "initializing localization system");

    g_loc_mutex = xSemaphoreCreateMutex();
    memset(&g_loc, 0, sizeof(g_loc));
    g_loc.initialized = true;
    g_loc.last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    g_loc.yaw_offset = 0.0f;

    /* Initialize pose with identity uncertainty */
    for (int i = 0; i < 6; i++) {
        g_loc.pose_covariance[i] = (pose_t){0};
    }
    g_loc.pose_covariance[0].x_cm = 25.0f;  /* 5cm std */
    g_loc.pose_covariance[1].x_cm = 25.0f;
    g_loc.pose_covariance[2].x_cm = 25.0f;  /* 5 deg std */

    ESP_LOGI(TAG, "localization ready (50Hz, wheelbase=%.1fcm, max_speed=%.0fcm/s)",
             LOC_WHEELBASE_CM, LOC_MAX_SPEED_CM_S);
}

bool localization_get_pose(pose_t *out) {
    if (!out || !g_loc.initialized) return false;

    if (xSemaphoreTake(g_loc_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        *out = g_loc.pose;
        xSemaphoreGive(g_loc_mutex);
        return true;
    }
    return false;
}

void localization_reset_origin(void) {
    if (xSemaphoreTake(g_loc_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        /* Reset position but keep current heading as forward */
        float current_yaw = g_loc.pose.yaw_deg;
        memset(&g_loc.pose, 0, sizeof(pose_t));
        g_loc.pose.yaw_deg = current_yaw;
        g_loc.yaw_offset = 0.0f;
        for (int i = 0; i < 6; i++) {
            g_loc.pose_covariance[i] = (pose_t){0};
        }
        g_loc.pose_covariance[0].x_cm = 25.0f;
        g_loc.pose_covariance[1].x_cm = 25.0f;
        g_loc.pose_covariance[2].x_cm = 25.0f;
        xSemaphoreGive(g_loc_mutex);
        ESP_LOGI(TAG, "origin reset, heading=%.1f deg", current_yaw);
    }
}

void localization_set_pose(const pose_t *pose) {
    if (!pose) return;

    if (xSemaphoreTake(g_loc_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_loc.pose = *pose;
        g_loc.yaw_offset = 0.0f;
        xSemaphoreGive(g_loc_mutex);
        ESP_LOGI(TAG, "pose set: x=%.1f y=%.1f yaw=%.1f",
                 pose->x_cm, pose->y_cm, pose->yaw_deg);
    }
}

bool localization_is_stopped(void) {
    return g_loc.zero_velocity;
}

/* ── Localization task ───────────────────────────────────────── */

void task_localization(void *arg) {
    ESP_LOGI(TAG, "localization task started (50Hz)");

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(1000 / LOC_TASK_FREQ_HZ);
    const float dt_s = 1.0f / (float)LOC_TASK_FREQ_HZ;

    /* Track the latest motor command for odometry */
    motor_cmd_t current_cmd = { .type = MOTOR_CMD_STOP, .move.direction = BCP_DIR_STOP, .move.speed = 0, .duration_ms = 0 };

    while (1) {
        /* ── 1. Get latest motor command (non-blocking) ── */
        motor_cmd_t cmd;
        if (xQueueReceive(q_cmd_incoming, &cmd, 0) == pdTRUE) {
            current_cmd = cmd;
        }

        /* ── 2. Get IMU data ── */
        imu_data_t imu;
        bool imu_ok = imu_get_latest(&imu);
        float imu_yaw_rate = 0.0f;
        float imu_accel_mag = 0.0f;
        if (imu_ok) {
            imu_yaw_rate = imu.gyro_z;  /* rad/s → but we want deg/s */
            imu_yaw_rate = rad_to_deg(imu_yaw_rate);
            imu_accel_mag = sqrtf(sq(imu.accel_x) + sq(imu.accel_y) + sq(imu.accel_z));
        }

        /* ── 3. Compute odometry from motor command ── */
        float odom_vx, odom_vy, odom_vyaw;
        command_to_velocity(&current_cmd, &odom_vx, &odom_vy, &odom_vyaw);

        /* ── 4. Zero-velocity detection ── */
        detect_zero_velocity(odom_vx, odom_vy, odom_vyaw, imu_accel_mag);

        /* ── 5. EKF predict ── */
        if (xSemaphoreTake(g_loc_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            ekf_predict(dt_s, odom_vx, odom_vy, odom_vyaw, imu_yaw_rate);
            g_loc.pose.timestamp_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            g_loc.last_update_ms = g_loc.pose.timestamp_ms;

            if (g_loc.zero_velocity) {
                g_loc.zero_velocity_duration_ms += (uint32_t)(dt_s * 1000.0f);
            }
            xSemaphoreGive(g_loc_mutex);
        }

        /* ── 6. ToF correction (at lower rate) ── */
        static int tof_counter = 0;
        tof_counter++;
        if (tof_counter >= 5) {  /* every 5 cycles = 10Hz */
            tof_counter = 0;
            if (xSemaphoreTake(g_loc_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                apply_tof_correction();
                xSemaphoreGive(g_loc_mutex);
            }
        }

        /* ── 7. Publish pose as BCP telemetry ── */
        /* Pose is published via IMU data or separate telemetry channel.
         * The behavior engine reads pose from localization_get_pose(). */

        vTaskDelayUntil(&last_wake, period);
    }
}
