/**
 * IMU driver — BMI270 6-axis over I2C.
 *
 * 200Hz accelerometer + gyroscope readout.
 * Complementary filter (α=0.98) for pitch/roll attitude.
 * Tap detection via INT1 GPIO interrupt.
 */

#include "imu.h"
#include "config.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_task_wdt.h"
#include <math.h>
#include <string.h>

static const char *TAG = "imu";

/* ── BMI270 Register map ──────────────────────────────────────── */

#define BMI270_CHIP_ID          0x00
#define BMI270_CHIP_ID_VAL      0x24
#define BMI270_ERR_REG          0x02
#define BMI270_STATUS           0x03
#define BMI270_ACC_DATA         0x0C  /* 6 bytes: X_L, X_H, Y_L, Y_H, Z_L, Z_H */
#define BMI270_GYR_DATA         0x12  /* 6 bytes: X_L, X_H, Y_L, Y_H, Z_L, Z_H */
#define BMI270_CMD              0x7E
#define BMI270_PWR_CTRL         0x7D
#define BMI270_ACC_CONF         0x40
#define BMI270_GYR_CONF         0x42
#define BMI270_INT1_IO_CTRL     0x53
#define BMI270_INT_MAP1         0x56

/* Commands */
#define CMD_SOFT_RESET          0xB6
#define CMD_ACC_NORMAL          0x11
#define CMD_GYR_NORMAL          0x15

/* ── I2C helpers ──────────────────────────────────────────────── */

static esp_err_t bmi270_write(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    return i2c_master_write_to_device(I2C_PORT, BMI270_ADDR, buf, 2, pdMS_TO_TICKS(10));
}

static esp_err_t bmi270_read(uint8_t reg, uint8_t *data, size_t len) {
    return i2c_master_write_read_device(I2C_PORT, BMI270_ADDR,
                                        &reg, 1, data, len, pdMS_TO_TICKS(10));
}

/* ── IMU state ────────────────────────────────────────────────── */

static imu_data_t g_imu_data;
static float g_cal_offset[6];  /* acc_x, acc_y, acc_z, gyr_x, gyr_y, gyr_z */
static portMUX_TYPE g_spinlock = portMUX_INITIALIZER_UNLOCKED;

/* ── Complementary filter state ───────────────────────────────── */

#define FILTER_ALPHA 0.98f  /* gyro trust: 98%, accel: 2% */

static float g_pitch, g_roll, g_yaw;
static int64_t g_last_gyro_us;

/* ── Public API ───────────────────────────────────────────────── */

void imu_init(void) {
    ESP_LOGI(TAG, "initializing BMI270 IMU (I2C 0x%02X)", BMI270_ADDR);

    uint8_t chip_id = 0;
    esp_err_t err;

    /* Verify chip ID */
    err = bmi270_read(BMI270_CHIP_ID, &chip_id, 1);
    if (err != ESP_OK || chip_id != BMI270_CHIP_ID_VAL) {
        ESP_LOGW(TAG, "BMI270 not found (err=%d, id=0x%02X) — using zero data", err, chip_id);
        g_imu_data = (imu_data_t){0};
        memset(g_cal_offset, 0, sizeof(g_cal_offset));
        return;
    }
    ESP_LOGI(TAG, "BMI270 chip ID: 0x%02X", chip_id);

    /* Soft reset */
    bmi270_write(BMI270_CMD, CMD_SOFT_RESET);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Configure accelerometer: ±4g, 200Hz ODR, normal mode */
    bmi270_write(BMI270_ACC_CONF, 0xA8);  /* ODR=200Hz, BW=normal, range=±4g */

    /* Configure gyroscope: ±500dps, 200Hz ODR */
    bmi270_write(BMI270_GYR_CONF, 0xA8);  /* ODR=200Hz, BW=normal, range=±500dps */

    /* Power on accelerometer and gyroscope */
    bmi270_write(BMI270_PWR_CTRL, 0x0E);  /* acc + gyr enabled */

    /* Configure INT1 for tap detection */
    bmi270_write(BMI270_INT1_IO_CTRL, 0x0A);  /* INT1 push-pull, active high */
    bmi270_write(BMI270_INT_MAP1, 0x40);       /* tap interrupt → INT1 */

    vTaskDelay(pdMS_TO_TICKS(10));

    /* Load calibration offsets from NVS */
    /* TODO: nvs_config_get_imu_cal(g_cal_offset) — Phase 3 */
    memset(g_cal_offset, 0, sizeof(g_cal_offset));

    /* Initialize attitude */
    g_pitch = 0.0f;
    g_roll  = 0.0f;
    g_yaw   = 0.0f;
    g_last_gyro_us = esp_timer_get_time();

    ESP_LOGI(TAG, "BMI270 initialized (accel ±4g, gyro ±500dps, 200Hz)");
}

bool imu_get_latest(imu_data_t *out) {
    if (!out) return false;

    portENTER_CRITICAL(&g_spinlock);
    *out = g_imu_data;
    portEXIT_CRITICAL(&g_spinlock);

    return true;
}

/* ── IMU task ─────────────────────────────────────────────────── */

void task_imu(void *arg) {
    ESP_LOGI(TAG, "IMU task started (200Hz)");

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(5);  /* ~200Hz */
    const float dt = 0.005f;

    while (1) {
        uint8_t raw[12];
        esp_err_t err = bmi270_read(BMI270_ACC_DATA, raw, 12);

        if (err == ESP_OK) {
            /* Parse accelerometer (little-endian, 16-bit signed, ±4g → 8192 LSB/g) */
            int16_t acc_x = (int16_t)(raw[0] | (raw[1] << 8));
            int16_t acc_y = (int16_t)(raw[2] | (raw[3] << 8));
            int16_t acc_z = (int16_t)(raw[4] | (raw[5] << 8));

            /* Parse gyroscope (little-endian, 16-bit signed, ±500dps → 65.5 LSB/dps) */
            int16_t gyr_x = (int16_t)(raw[6] | (raw[7] << 8));
            int16_t gyr_y = (int16_t)(raw[8] | (raw[9] << 8));
            int16_t gyr_z = (int16_t)(raw[10] | (raw[11] << 8));

            /* Convert to physical units */
            const float accel_scale = 4.0f * 9.80665f / 32768.0f;  /* m/s^2 per LSB */
            const float gyro_scale  = 500.0f * 3.14159265f / 180.0f / 32768.0f;  /* rad/s per LSB */

            float ax = acc_x * accel_scale - g_cal_offset[0];
            float ay = acc_y * accel_scale - g_cal_offset[1];
            float az = acc_z * accel_scale - g_cal_offset[2];
            float gx = gyr_x * gyro_scale  - g_cal_offset[3];
            float gy = gyr_y * gyro_scale  - g_cal_offset[4];
            float gz = gyr_z * gyro_scale  - g_cal_offset[5];

            /* Deadband: suppress noise below threshold */
            if (fabsf(gx) < 0.01f) gx = 0.0f;
            if (fabsf(gy) < 0.01f) gy = 0.0f;
            if (fabsf(gz) < 0.01f) gz = 0.0f;

            /* Complementary filter for pitch and roll */
            float acc_pitch = atan2f(-ax, sqrtf(ay * ay + az * az)) * 180.0f / 3.14159265f;
            float acc_roll  = atan2f(ay, az) * 180.0f / 3.14159265f;

            /* Integrate gyro for yaw (simplified — drift accumulates, needs mag for correction) */
            uint64_t now_us = esp_timer_get_time();
            float gyro_dt = (now_us - g_last_gyro_us) / 1000000.0f;
            g_last_gyro_us = now_us;

            if (gyro_dt > 0.0f && gyro_dt < 0.1f) {
                g_pitch = FILTER_ALPHA * (g_pitch + gx * gyro_dt * 180.0f / 3.14159265f)
                        + (1.0f - FILTER_ALPHA) * acc_pitch;
                g_roll  = FILTER_ALPHA * (g_roll  + gy * gyro_dt * 180.0f / 3.14159265f)
                        + (1.0f - FILTER_ALPHA) * acc_roll;
                g_yaw  += gz * gyro_dt * 180.0f / 3.14159265f;
            }

            /* Update global state */
            portENTER_CRITICAL(&g_spinlock);
            g_imu_data.accel_x = ax;
            g_imu_data.accel_y = ay;
            g_imu_data.accel_z = az;
            g_imu_data.gyro_x  = gx;
            g_imu_data.gyro_y  = gy;
            g_imu_data.gyro_z  = gz;
            g_imu_data.pitch   = g_pitch;
            g_imu_data.roll    = g_roll;
            g_imu_data.yaw     = g_yaw;
            g_imu_data.timestamp_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            portEXIT_CRITICAL(&g_spinlock);
        } else {
            /* Don't flood logs on transient I2C errors */
            static int err_count = 0;
            if (++err_count % 100 == 0) {
                ESP_LOGW(TAG, "I2C read error: %d (count=%d)", err, err_count);
            }
        }

        vTaskDelayUntil(&last_wake, period);
        esp_task_wdt_reset();
    }
}
