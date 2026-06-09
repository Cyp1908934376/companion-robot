/**
 * Localization — Dead Reckoning + ToF Correction for MiniBot.
 *
 * Fuses motor odometry (open-loop commanded velocity), IMU yaw, and
 * ToF wall-distance measurements into a position estimate (x, y, yaw).
 *
 * Since the N20 motors lack encoders, odometry is derived from commanded
 * wheel speeds via mecanum kinematics. IMU gyro integration provides yaw
 * with zero-rotation detection to limit drift. ToF sensors provide
 * periodic wall-distance corrections.
 *
 * State vector (6D): [x_cm, y_cm, yaw_deg, vx_cm_s, vy_cm_s, vyaw_deg_s]
 * Reference frame: +X forward, +Y left, +yaw CCW (robot-centric at t=0)
 */

#ifndef LOCALIZATION_H
#define LOCALIZATION_H

#include "imu.h"
#include "motor.h"
#include "tof.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Configuration ─────────────────────────────────────────── */

#define LOC_TASK_FREQ_HZ         50    /* localization update rate */
#define LOC_WHEELBASE_CM         12.0f /* half wheelbase for rotation */
#define LOC_MAX_SPEED_CM_S       40.0f /* max speed in cm/s */
#define LOC_ZERO_VEL_THRESH      0.02f /* below this = stopped */
#define LOC_TOF_CORRECT_GAIN     0.3f  /* ToF correction blending */
#define LOC_TOF_MAX_DIST_CM      200   /* max useful ToF range */

/* Measurement noise (standard deviations) */
#define LOC_R_ODOM_VX            3.0f  /* cm/s */
#define LOC_R_ODOM_VYAW          5.0f  /* deg/s */
#define LOC_R_IMU_YAW            1.0f  /* deg */
#define LOC_R_TOF                5.0f  /* cm */

/* Process noise */
#define LOC_Q_POS                0.5f  /* cm */
#define LOC_Q_YAW                0.3f  /* deg */
#define LOC_Q_VEL                1.0f  /* cm/s^2 */

/* ── Types ──────────────────────────────────────────────────── */

/** Robot pose in world frame. */
typedef struct {
    float x_cm;        /* forward+ position */
    float y_cm;        /* left+ position */
    float yaw_deg;     /* heading, CCW positive, 0 = forward */
    float vx_cm_s;     /* forward velocity */
    float vy_cm_s;     /* lateral velocity */
    float vyaw_deg_s;  /* angular velocity */
    uint32_t timestamp_ms;
} pose_t;

/** Localization task state. */
typedef struct {
    pose_t pose;
    pose_t pose_covariance[6]; /* diagonal covariance */
    uint32_t last_update_ms;
    uint32_t last_tof_correct_ms;
    bool initialized;
    bool zero_velocity;
    uint32_t zero_velocity_duration_ms;
    float yaw_offset;    /* transform IMU yaw → world yaw */
} localization_t;

/* ── API ────────────────────────────────────────────────────── */

/** Initialize the localization system. */
void localization_init(void);

/** Get the latest pose estimate. Returns false if not yet initialized. */
bool localization_get_pose(pose_t *out);

/** Reset position to origin (0,0,0) with current heading as forward. */
void localization_reset_origin(void);

/** Set a known pose (e.g., from charging station dock). */
void localization_set_pose(const pose_t *pose);

/** Check if the robot is currently stationary. */
bool localization_is_stopped(void);

/** Localization task (Core 1, 50Hz). */
void task_localization(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* LOCALIZATION_H */
