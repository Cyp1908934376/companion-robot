/**
 * Hardware pin definitions and system parameters.
 * Phase 1 MiniBot configuration (ESP32-S3-WROOM-1 N16R8).
 */

#ifndef CONFIG_H
#define CONFIG_H

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── I2C Bus ──────────────────────────────────────────────── */
#define I2C_MASTER_SCL_IO       22
#define I2C_MASTER_SDA_IO       21
#define I2C_MASTER_FREQ_HZ      400000
#define I2C_PORT                I2C_NUM_0

/* ── I2S Microphone (INMP441 array, 4ch) ─────────────────── */
#define I2S_MIC_BCK_IO          26
#define I2S_MIC_WS_IO           25
#define I2S_MIC_DATA_IO         33
#define I2S_MIC_PORT            I2S_NUM_0
#define I2S_MIC_SAMPLE_RATE     16000
#define I2S_MIC_BITS_PER_SAMPLE I2S_BITS_PER_SAMPLE_16BIT

/* ── I2S Speaker (MAX98357A) ──────────────────────────────── */
#define I2S_SPK_BCK_IO          14
#define I2S_SPK_WS_IO           27
#define I2S_SPK_DATA_IO         13
#define I2S_SPK_PORT            I2S_NUM_1
#define I2S_SPK_SAMPLE_RATE     22050

/* ── Motors (DRV8833 x2, 4x N20 micrometal) ──────────────── */
#define MOTOR_L1_A_IO           32
#define MOTOR_L1_B_IO           33
#define MOTOR_L2_A_IO           25
#define MOTOR_L2_B_IO           26
#define MOTOR_R1_A_IO           27
#define MOTOR_R1_B_IO           14
#define MOTOR_R2_A_IO           12
#define MOTOR_R2_B_IO           13

#define MOTOR_PWM_FREQ_HZ       20000
#define MOTOR_PWM_RESOLUTION    LEDC_TIMER_10_BIT
#define MOTOR_CONTROL_FREQ_HZ   100  /* 100Hz PID loop */

/* ── Servos (head pan/tilt) ────────────────────────────────── */
#define SERVO_PAN_IO            18
#define SERVO_TILT_IO           19
#define SERVO_PWM_FREQ_HZ       50
#define SERVO_MIN_US            500
#define SERVO_MAX_US            2500

/* ── WS2812B LEDs ─────────────────────────────────────────── */
#define LED_DATA_IO             5
#define LED_COUNT               8
#define LED_RMT_CHANNEL         RMT_CHANNEL_0

/* ── Camera (OV2640, SPI) ─────────────────────────────────── */
#define CAM_SCK_IO              40
#define CAM_MISO_IO             41
#define CAM_MOSI_IO             42
#define CAM_CS_IO               44

/* ── ToF Sensors (VL53L0X x3, I2C addr switching) ─────────── */
#define TOF_XSHUT_1_IO          34
#define TOF_XSHUT_2_IO          35
#define TOF_XSHUT_3_IO          36

/* ── Capacitive Touch ─────────────────────────────────────── */
#define TOUCH_ZONE_HEAD         TOUCH_PAD_NUM9   /* GPIO32 */
#define TOUCH_ZONE_BACK         TOUCH_PAD_NUM8   /* GPIO33 */
#define TOUCH_ZONE_LEFT         TOUCH_PAD_NUM7   /* GPIO27 */
#define TOUCH_ZONE_RIGHT        TOUCH_PAD_NUM6   /* GPIO14 */

/* ── I2C Sensor Addresses ──────────────────────────────────── */
#define BMI270_ADDR             0x68  /* IMU */
#define BME280_ADDR             0x76  /* Env: temp/humi/pressure */
#define SGP30_ADDR              0x58  /* Air quality */
#define BH1750_ADDR             0x23  /* Ambient light */
#define MAX17048_ADDR           0x36  /* Battery fuel gauge */
#define VL53L0X_ADDR            0x29  /* ToF (shared, XSHUT muxed) */

/* ── Task Stack Sizes (words) ─────────────────────────────── */
#define STACK_COMM_RX           4096
#define STACK_COMM_TX           4096
#define STACK_CONN_MGR          4096
#define STACK_VISION            8192
#define STACK_AUDIO_IN          8192
#define STACK_AUDIO_OUT         4096
#define STACK_IMU               2048
#define STACK_MOTOR             4096
#define STACK_SERVO             2048
#define STACK_ENV_SENSOR        2048
#define STACK_TOUCH             2048
#define STACK_LED               2048
#define STACK_LOCALIZATION       4096
#define STACK_BEHAVIOR          4096
#define STACK_POWER             2048

/* ── Task Priorities (higher = more urgent) ────────────────── */
#define PRIO_COMM_RX            5
#define PRIO_COMM_TX            4
#define PRIO_CONN_MGR           3
#define PRIO_AUDIO_IN           5
#define PRIO_VISION             4
#define PRIO_MOTOR              5
#define PRIO_TOUCH              4
#define PRIO_LOCALIZATION       3
#define PRIO_IMU                3
#define PRIO_AUDIO_OUT          3
#define PRIO_SERVO              3
#define PRIO_LED                2
#define PRIO_BEHAVIOR           2
#define PRIO_ENV_SENSOR         1
#define PRIO_POWER              1

/* ── Queue Depths ─────────────────────────────────────────── */
#define Q_CMD_INCOMING_DEPTH    16
#define Q_CMD_OUTGOING_DEPTH    32
#define Q_SENSOR_EVENT_DEPTH    8
#define Q_AUDIO_FRAME_DEPTH     4
#define Q_VISION_FRAME_DEPTH    2

/* ── Timing ────────────────────────────────────────────────── */
#define HEARTBEAT_INTERVAL_MS   5000
#define WATCHDOG_TIMEOUT_MS     2000
#define WIFI_CONNECT_TIMEOUT_MS 10000
#define BLE_ADV_TIMEOUT_MS      15000

/* ── Sleep / Low-Power Timing ────────────────────────────────── */
#define IDLE_TIMEOUT_MS         300000   /* 5min no task → IDLE (disable camera) */
#define LIGHT_SLEEP_TIMEOUT_MS  1800000  /* 30min idle → Light Sleep */
#define DEEP_SLEEP_TIMEOUT_MS   3600000  /* 60min → Deep Sleep (if battery < 20%) */
#define LIGHT_SLEEP_WAKE_MS     5000     /* wake every 5s for heartbeat check */
#define DEEP_SLEEP_WAKE_SEC     60       /* wake every 60s for status check */

/* ── Battery Thresholds ────────────────────────────────────── */
#define BATTERY_LOW_PCT         20
#define BATTERY_CRITICAL_PCT    10
#define BATTERY_EXIT_CHARGE_PCT 80

/* ── Obstacle Thresholds (cm) ──────────────────────────────── */
#define OBSTACLE_SAFE_CM        50
#define OBSTACLE_SLOW_CM        30
#define OBSTACLE_STOP_CM        10

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
