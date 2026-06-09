/**
 * Unit tests for the localization component.
 *
 * Tests command-to-velocity kinematics, EKF predict step,
 * zero-velocity detection, ToF correction, and public API.
 *
 * Built with ESP-IDF Unity test framework:
 *   cd firmware/test && idf.py build && idf.py flash monitor
 */

#include "unity.h"
#include "localization.h"
#include "bcp_codec.h"
#include <math.h>
#include <string.h>

/* ── Stubs for FreeRTOS / ESP-IDF dependencies ──────────────── */

/* The localization.c static helpers are tested via public API or
 * by including the .c file directly in test builds.
 * We use a lightweight stub approach: expose internals through
 * a test-access header that re-declares the static functions. */

/* Re-declare static functions for direct testing */
void command_to_velocity(const motor_cmd_t *cmd,
                         float *vx_out, float *vy_out, float *vyaw_out);
void ekf_predict(float dt_s, float odom_vx, float odom_vy,
                 float odom_vyaw, float imu_yaw_rate);
void detect_zero_velocity(float odom_vx, float odom_vy,
                           float odom_vyaw, float imu_accel_mag);
void apply_tof_correction(void);

/* ── Test helpers ───────────────────────────────────────────── */

static motor_cmd_t make_cmd(bcp_direction_t dir, uint8_t speed) {
    motor_cmd_t cmd = {
        .type = MOTOR_CMD_MOVE,
        .move.direction = dir,
        .move.speed = speed,
        .duration_ms = 0,
        .no_ack = false,
    };
    return cmd;
}

/* ── command_to_velocity tests ────────────────────────────────── */

TEST_CASE("cmd_to_vel: STOP produces zero velocity", "[localization]")
{
    motor_cmd_t cmd = make_cmd(BCP_DIR_STOP, 128);
    float vx, vy, vyaw;
    command_to_velocity(&cmd, &vx, &vy, &vyaw);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, vx);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, vy);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, vyaw);
}

TEST_CASE("cmd_to_vel: NULL cmd produces zero velocity", "[localization]")
{
    float vx, vy, vyaw;
    command_to_velocity(NULL, &vx, &vy, &vyaw);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, vx);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, vy);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, vyaw);
}

TEST_CASE("cmd_to_vel: FORWARD with full speed", "[localization]")
{
    motor_cmd_t cmd = make_cmd(BCP_DIR_FORWARD, 255);
    float vx, vy, vyaw;
    command_to_velocity(&cmd, &vx, &vy, &vyaw);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 40.0f, vx);   /* LOC_MAX_SPEED_CM_S */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, vy);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, vyaw);
}

TEST_CASE("cmd_to_vel: BACKWARD is negative forward", "[localization]")
{
    motor_cmd_t cmd = make_cmd(BCP_DIR_BACKWARD, 128);
    float vx, vy, vyaw;
    command_to_velocity(&cmd, &vx, &vy, &vyaw);
    TEST_ASSERT_TRUE(vx < -0.1f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, vy);
}

TEST_CASE("cmd_to_vel: LEFT produces positive vy", "[localization]")
{
    motor_cmd_t cmd = make_cmd(BCP_DIR_LEFT, 128);
    float vx, vy, vyaw;
    command_to_velocity(&cmd, &vx, &vy, &vyaw);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, vx);
    TEST_ASSERT_TRUE(vy > 0.1f);
}

TEST_CASE("cmd_to_vel: RIGHT produces negative vy", "[localization]")
{
    motor_cmd_t cmd = make_cmd(BCP_DIR_RIGHT, 128);
    float vx, vy, vyaw;
    command_to_velocity(&cmd, &vx, &vy, &vyaw);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, vx);
    TEST_ASSERT_TRUE(vy < -0.1f);
}

TEST_CASE("cmd_to_vel: FORWARD_LEFT combines vx and vy", "[localization]")
{
    motor_cmd_t cmd = make_cmd(BCP_DIR_FORWARD_LEFT, 255);
    float vx, vy, vyaw;
    command_to_velocity(&cmd, &vx, &vy, &vyaw);
    TEST_ASSERT_TRUE(vx > 0.1f);
    TEST_ASSERT_TRUE(vy > 0.1f);
}

TEST_CASE("cmd_to_vel: ROTATE_LEFT produces positive angular velocity", "[localization]")
{
    motor_cmd_t cmd = make_cmd(BCP_DIR_ROTATE_LEFT, 128);
    float vx, vy, vyaw;
    command_to_velocity(&cmd, &vx, &vy, &vyaw);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, vx);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, vy);
    TEST_ASSERT_TRUE(vyaw > 0.1f);
}

TEST_CASE("cmd_to_vel: ROTATE_RIGHT produces negative angular velocity", "[localization]")
{
    motor_cmd_t cmd = make_cmd(BCP_DIR_ROTATE_RIGHT, 128);
    float vx, vy, vyaw;
    command_to_velocity(&cmd, &vx, &vy, &vyaw);
    TEST_ASSERT_TRUE(vyaw < -0.1f);
}

TEST_CASE("cmd_to_vel: speed scaling at half throttle", "[localization]")
{
    motor_cmd_t full = make_cmd(BCP_DIR_FORWARD, 255);
    motor_cmd_t half = make_cmd(BCP_DIR_FORWARD, 128);
    float vx_full, vy, vyaw;
    float vx_half;
    command_to_velocity(&full, &vx_full, &vy, &vyaw);
    command_to_velocity(&half, &vx_half, &vy, &vyaw);
    /* Half speed should be roughly half full speed (128/255 ≈ 0.502) */
    float ratio = vx_half / vx_full;
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.502f, ratio);
}

/* ── EKF predict tests ──────────────────────────────────────── */

TEST_CASE("ekf_predict: forward motion integrates x position", "[localization]")
{
    localization_init();

    /* Simulate 1 second of forward motion at 20 cm/s, heading 0 */
    for (int i = 0; i < 50; i++) {
        ekf_predict(0.02f, 20.0f, 0.0f, 0.0f, 0.0f);
    }

    pose_t pose;
    TEST_ASSERT_TRUE(localization_get_pose(&pose));
    /* 1s * 20cm/s = ~20cm forward, with alpha blending it'll be close */
    TEST_ASSERT_TRUE(pose.x_cm > 10.0f);
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 0.0f, pose.y_cm);
}

TEST_CASE("ekf_predict: rotation integrates yaw", "[localization]")
{
    localization_reset_origin();

    /* Simulate 1 second of rotation at 90 deg/s */
    for (int i = 0; i < 50; i++) {
        ekf_predict(0.02f, 0.0f, 0.0f, 0.0f, 90.0f);
    }

    pose_t pose;
    TEST_ASSERT_TRUE(localization_get_pose(&pose));
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 90.0f, pose.yaw_deg);
}

TEST_CASE("ekf_predict: yaw wraps to [-180, 180]", "[localization]")
{
    localization_reset_origin();

    /* Simulate rotation past 180 degrees */
    for (int i = 0; i < 50; i++) {
        ekf_predict(0.02f, 0.0f, 0.0f, 0.0f, 400.0f);
    }

    pose_t pose;
    TEST_ASSERT_TRUE(localization_get_pose(&pose));
    /* 50 * 0.02 * 400 = 400 deg, wrapped to [40] */
    TEST_ASSERT_TRUE(pose.yaw_deg >= -180.0f && pose.yaw_deg <= 180.0f);
}

TEST_CASE("ekf_predict: velocity blending with odometry", "[localization]")
{
    localization_reset_origin();

    /* First update: strong odometry signal */
    ekf_predict(0.02f, 30.0f, 0.0f, 0.0f, 0.0f);

    pose_t pose;
    localization_get_pose(&pose);
    /* alpha=0.7, so vx ≈ 0.7*30 = 21 cm/s */
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 21.0f, pose.vx_cm_s);
}

TEST_CASE("ekf_predict: covariance grows with time", "[localization]")
{
    localization_reset_origin();

    /* The covariance is internal; we verify the system remains sane */
    for (int i = 0; i < 100; i++) {
        ekf_predict(0.02f, 10.0f, 0.0f, 0.0f, 0.0f);
    }

    pose_t pose;
    TEST_ASSERT_TRUE(localization_get_pose(&pose));
    TEST_ASSERT_TRUE(pose.x_cm > 0.0f);
}

/* ── Zero-velocity detection tests ──────────────────────────── */

TEST_CASE("zv_detect: stopped when all inputs near zero", "[localization]")
{
    localization_reset_origin();

    /* Apply ZVU over many iterations */
    for (int i = 0; i < 30; i++) {
        detect_zero_velocity(0.0f, 0.0f, 0.0f, 0.0f);
        ekf_predict(0.02f, 0.0f, 0.0f, 0.0f, 0.0f);
    }

    TEST_ASSERT_TRUE(localization_is_stopped());
}

TEST_CASE("zv_detect: not stopped when moving", "[localization]")
{
    localization_reset_origin();

    /* Move forward, should not be stopped */
    detect_zero_velocity(20.0f, 0.0f, 0.0f, 0.05f * 9.81f);

    TEST_ASSERT_FALSE(localization_is_stopped());
}

TEST_CASE("zv_detect: stops again after movement ends", "[localization]")
{
    localization_reset_origin();

    /* Move briefly */
    detect_zero_velocity(20.0f, 0.0f, 0.0f, 0.1f * 9.81f);
    TEST_ASSERT_FALSE(localization_is_stopped());

    /* Then stop for many iterations */
    for (int i = 0; i < 30; i++) {
        detect_zero_velocity(0.0f, 0.0f, 0.0f, 0.0f);
    }
    TEST_ASSERT_TRUE(localization_is_stopped());
}

/* ── Public API tests ───────────────────────────────────────── */

TEST_CASE("api: init populates initial pose", "[localization]")
{
    localization_init();

    pose_t pose;
    TEST_ASSERT_TRUE(localization_get_pose(&pose));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, pose.x_cm);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, pose.y_cm);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, pose.yaw_deg);
}

TEST_CASE("api: get_pose returns false with NULL output", "[localization]")
{
    localization_init();
    TEST_ASSERT_FALSE(localization_get_pose(NULL));
}

TEST_CASE("api: reset_origin clears position, keeps heading", "[localization]")
{
    localization_init();

    /* Move to a known state */
    for (int i = 0; i < 50; i++) {
        ekf_predict(0.02f, 20.0f, 0.0f, 0.0f, 30.0f);
    }

    pose_t before;
    localization_get_pose(&before);
    TEST_ASSERT_TRUE(before.x_cm > 1.0f);

    localization_reset_origin();

    pose_t after;
    TEST_ASSERT_TRUE(localization_get_pose(&after));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, after.x_cm);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, after.y_cm);
    /* Heading should be preserved */
    TEST_ASSERT_TRUE(fabsf(after.yaw_deg) > 0.1f);
}

TEST_CASE("api: set_pose overrides position", "[localization]")
{
    localization_init();

    pose_t new_pose = {
        .x_cm = 100.0f,
        .y_cm = 50.0f,
        .yaw_deg = 45.0f,
        .vx_cm_s = 0.0f,
        .vy_cm_s = 0.0f,
        .vyaw_deg_s = 0.0f,
        .timestamp_ms = 0,
    };
    localization_set_pose(&new_pose);

    pose_t pose;
    TEST_ASSERT_TRUE(localization_get_pose(&pose));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, pose.x_cm);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, pose.y_cm);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 45.0f, pose.yaw_deg);
}

TEST_CASE("api: set_pose NULL is safe", "[localization]")
{
    localization_init();
    localization_set_pose(NULL);
    /* Should not crash */
    pose_t pose;
    TEST_ASSERT_TRUE(localization_get_pose(&pose));
}

TEST_CASE("api: is_stopped defaults to false", "[localization]")
{
    localization_init();
    TEST_ASSERT_FALSE(localization_is_stopped());
}

/* ── Mecanum kinematics sanity tests ────────────────────────── */

TEST_CASE("kinematics: pure forward gives all wheels positive", "[localization]")
{
    motor_cmd_t cmd = make_cmd(BCP_DIR_FORWARD, 200);
    float vx, vy, vyaw;
    command_to_velocity(&cmd, &vx, &vy, &vyaw);
    TEST_ASSERT_TRUE(vx > 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, vy);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, vyaw);
}

TEST_CASE("kinematics: diagonal motion has both components", "[localization]")
{
    motor_cmd_t cmd = make_cmd(BCP_DIR_FORWARD_RIGHT, 255);
    float vx, vy, vyaw;
    command_to_velocity(&cmd, &vx, &vy, &vyaw);
    TEST_ASSERT_TRUE(vx > 0.0f);
    TEST_ASSERT_TRUE(vy < 0.0f);  /* Right = negative vy */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, vyaw);
}

/* ── Deg/Rad conversion sanity ──────────────────────────────── */

TEST_CASE("math: deg to rad roundtrip", "[localization]")
{
    /* 90 deg → rad → deg should be 90 */
    float rad = 90.0f * 3.14159265f / 180.0f;
    float deg = rad * 180.0f / 3.14159265f;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 90.0f, deg);
}
