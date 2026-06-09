/**
 * ToF distance sensors — VL53L0X x3 over I2C (XSHUT-muxed).
 *
 * Three sensors provide front/side obstacle detection.
 * Each sensor is assigned a unique I2C address via XSHUT sequencing.
 * Range: 30mm–2000mm. Read on-demand via tof_read_all().
 */

#include "tof.h"
#include "config.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include <string.h>

static const char *TAG = "tof";

/* ── VL53L0X Register map ─────────────────────────────────────── */

#define VL53L0X_REG_IDENTIFICATION_MODEL_ID    0xC0
#define VL53L0X_ID_VAL                         0xEE
#define VL53L0X_REG_SYSRANGE_START             0x00
#define VL53L0X_REG_RESULT_RANGE_STATUS        0x14
#define VL53L0X_REG_SYSTEM_INTERRUPT_CLEAR     0x0B
#define VL53L0X_REG_I2C_SLAVE_DEVICE_ADDRESS   0x8A
#define VL53L0X_REG_VHV_CONFIG_PAD_SCL_SDA     0x89

/* Assigned I2C addresses after XSHUT sequencing */
#define TOF_ADDR_1  0x30
#define TOF_ADDR_2  0x31
#define TOF_ADDR_3  0x32

/* Sensor XSHUT GPIOs */
static const gpio_num_t XSHUT_PINS[TOF_COUNT] = {
    TOF_XSHUT_1_IO,
    TOF_XSHUT_2_IO,
    TOF_XSHUT_3_IO,
};

static const uint8_t TOF_ADDRS[TOF_COUNT] = {
    TOF_ADDR_1, TOF_ADDR_2, TOF_ADDR_3,
};

/* ── Sensor state ─────────────────────────────────────────────── */

static bool g_tof_init_ok[TOF_COUNT];

/* ── I2C helpers ──────────────────────────────────────────────── */

static esp_err_t vl53l0x_write(uint8_t addr, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    return i2c_master_write_to_device(I2C_PORT, addr, buf, 2, pdMS_TO_TICKS(10));
}

static esp_err_t vl53l0x_read(uint8_t addr, uint8_t reg, uint8_t *data, size_t len) {
    return i2c_master_write_read_device(I2C_PORT, addr,
                                        &reg, 1, data, len, pdMS_TO_TICKS(10));
}

/* ── VL53L0X initialization sequence ──────────────────────────── */

static bool init_single_sensor(int index, uint8_t new_addr) {
    /* Release from XSHUT */
    gpio_set_level(XSHUT_PINS[index], 1);
    vTaskDelay(pdMS_TO_TICKS(2));

    /* Verify model ID (uses default address 0x29) */
    uint8_t model_id = 0;
    esp_err_t err = vl53l0x_read(VL53L0X_ADDR, VL53L0X_REG_IDENTIFICATION_MODEL_ID,
                                 &model_id, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ToF[%d]: I2C read error (err=0x%X)", index, err);
        return false;
    }
    if (model_id != VL53L0X_ID_VAL) {
        ESP_LOGW(TAG, "ToF[%d]: unexpected model ID 0x%02X", index, model_id);
        return false;
    }

    /* Set new I2C address */
    err = vl53l0x_write(VL53L0X_ADDR, VL53L0X_REG_I2C_SLAVE_DEVICE_ADDRESS,
                        new_addr & 0x7F);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ToF[%d]: failed to set I2C address", index);
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(1));

    /* Init sequence: enable VHV adjustment, set continuous ranging */
    vl53l0x_write(new_addr, VL53L0X_REG_VHV_CONFIG_PAD_SCL_SDA, 0x01);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Start continuous ranging */
    vl53l0x_write(new_addr, VL53L0X_REG_SYSRANGE_START, 0x02);

    ESP_LOGI(TAG, "ToF[%d]: initialized at I2C addr 0x%02X", index, new_addr);
    return true;
}

/* ── Range reading ────────────────────────────────────────────── */

static bool read_range(int index, uint16_t *out_mm) {
    uint8_t addr = TOF_ADDRS[index];

    /* Read range status + range */
    uint8_t status = 0;
    esp_err_t err = vl53l0x_read(addr, VL53L0X_REG_RESULT_RANGE_STATUS, &status, 1);
    if (err != ESP_OK) return false;

    /* Check data ready */
    if (!(status & 0x01)) {
        return false;  /* measurement not complete */
    }

    /* Check range status (bits 7:4). 0 = valid range */
    uint8_t range_status = (status >> 4) & 0x0F;
    if (range_status != 0) {
        *out_mm = 0;  /* out of range or error */
        /* Clear interrupt and return true (measurement was attempted) */
        vl53l0x_write(addr, VL53L0X_REG_SYSTEM_INTERRUPT_CLEAR, 0x01);
        return true;
    }

    /* Read range value (offset 10 from RESULT_RANGE_STATUS register) */
    uint8_t range_buf[2];
    err = vl53l0x_read(addr, VL53L0X_REG_RESULT_RANGE_STATUS + 10, range_buf, 2);
    if (err == ESP_OK) {
        *out_mm = ((uint16_t)range_buf[0] << 8) | range_buf[1];
    }

    /* Clear interrupt */
    vl53l0x_write(addr, VL53L0X_REG_SYSTEM_INTERRUPT_CLEAR, 0x01);

    return true;
}

/* ── Public API ───────────────────────────────────────────────── */

void tof_init(void) {
    ESP_LOGI(TAG, "initializing VL53L0X x3 ToF sensors");

    /* Configure XSHUT pins */
    for (int i = 0; i < TOF_COUNT; i++) {
        gpio_set_direction(XSHUT_PINS[i], GPIO_MODE_OUTPUT);
        gpio_set_pull_mode(XSHUT_PINS[i], GPIO_PULLDOWN_ONLY);
        gpio_set_level(XSHUT_PINS[i], 0);  /* hold all in reset */
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Initialize each sensor one at a time */
    for (int i = 0; i < TOF_COUNT; i++) {
        /* Hold all except current in reset */
        for (int j = 0; j < TOF_COUNT; j++) {
            gpio_set_level(XSHUT_PINS[j], (j == i) ? 1 : 0);
        }

        g_tof_init_ok[i] = init_single_sensor(i, TOF_ADDRS[i]);
    }

    /* Release all sensors for normal operation */
    for (int i = 0; i < TOF_COUNT; i++) {
        gpio_set_level(XSHUT_PINS[i], 1);
    }

    int ok_count = 0;
    for (int i = 0; i < TOF_COUNT; i++) {
        if (g_tof_init_ok[i]) ok_count++;
    }
    ESP_LOGI(TAG, "ToF initialization complete (%d/%d sensors OK)", ok_count, TOF_COUNT);
}

bool tof_read_all(tof_reading_t *out) {
    if (!out) return false;

    memset(out, 0, sizeof(*out));

    bool any_ok = false;
    for (int i = 0; i < TOF_COUNT; i++) {
        if (!g_tof_init_ok[i]) {
            out->distance_mm[i] = 0;
            continue;
        }
        uint16_t range;
        if (read_range(i, &range)) {
            out->distance_mm[i] = range;
            any_ok = true;
        } else {
            out->distance_mm[i] = 0;
        }
    }

    out->timestamp_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    return any_ok;
}
