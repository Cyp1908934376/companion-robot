/**
 * Environmental sensors — BME280 + SGP30 + BH1750 over I2C.
 *
 * Reads at 1Hz:
 *   - BME280: temperature, humidity, pressure
 *   - SGP30:  CO2 equivalent, TVOC
 *   - BH1750: ambient light lux
 */

#include "env_sensor.h"
#include "config.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include <string.h>
#include <math.h>

static const char *TAG = "env";

/* ── BME280 Register map ──────────────────────────────────────── */

#define BME280_REG_ID          0xD0
#define BME280_ID_VAL          0x60
#define BME280_REG_RESET       0xE0
#define BME280_RESET_CMD       0xB6
#define BME280_REG_CTRL_HUM    0xF2
#define BME280_REG_CTRL_MEAS   0xF4
#define BME280_REG_CONFIG      0xF5
#define BME280_REG_DATA        0xF7  /* 8 bytes: P[0..2] T[0..2] H[0..1] */

/* BME280 calibration (26 bytes from 0x88 + 7 bytes from 0xE1 + 8 bytes from 0xE4) */
#define BME280_REG_CALIB_1     0x88  /* 26 bytes */
#define BME280_REG_CALIB_2     0xE1  /* 7 bytes */
#define BME280_CALIB1_LEN      26
#define BME280_CALIB2_LEN      7

/* ── SGP30 Register map ───────────────────────────────────────── */

#define SGP30_REG_SERIAL_ID            0x3682
#define SGP30_REG_FEATURESET           0x202F
#define SGP30_REG_INIT_AIR_QUALITY     0x2003
#define SGP30_REG_MEASURE_AIR_QUALITY  0x2008
#define SGP30_REG_GET_BASELINE         0x2015
#define SGP30_REG_SET_BASELINE         0x201E
#define SGP30_REG_SET_HUMIDITY         0x2061

/* ── BH1750 Register map ──────────────────────────────────────── */

#define BH1750_POWER_ON        0x01
#define BH1750_RESET           0x07
#define BH1750_CONT_HRES_MODE  0x10  /* continuous high-res, 1lx resolution, 120ms */

/* ── State ────────────────────────────────────────────────────── */

static env_data_t g_env_data;
static portMUX_TYPE g_spinlock = portMUX_INITIALIZER_UNLOCKED;

static bool g_bme280_ok;
static bool g_sgp30_ok;
static bool g_bh1750_ok;

/* ── BME280 calibration data ──────────────────────────────────── */

typedef struct {
    uint16_t dig_T1;
    int16_t  dig_T2, dig_T3;
    uint16_t dig_P1;
    int16_t  dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
    uint8_t  dig_H1;
    int16_t  dig_H2;
    uint8_t  dig_H3;
    int16_t  dig_H4, dig_H5;
    int8_t   dig_H6;
} bme280_calib_t;

static bme280_calib_t g_bme_cal;
static int32_t g_t_fine;  /* for compensation formulas */

/* ── I2C helpers ──────────────────────────────────────────────── */

static esp_err_t i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    return i2c_master_write_to_device(I2C_PORT, addr, buf, 2, pdMS_TO_TICKS(10));
}

static esp_err_t i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *data, size_t len) {
    return i2c_master_write_read_device(I2C_PORT, addr, &reg, 1, data, len, pdMS_TO_TICKS(20));
}

/* ── BME280 compensation (Bosch datasheet) ────────────────────── */

static int32_t bme280_compensate_T(int32_t adc_T) {
    int32_t var1 = ((((adc_T >> 3) - ((int32_t)g_bme_cal.dig_T1 << 1)))
                    * ((int32_t)g_bme_cal.dig_T2)) >> 11;
    int32_t var2 = (((((adc_T >> 4) - ((int32_t)g_bme_cal.dig_T1))
                      * ((adc_T >> 4) - ((int32_t)g_bme_cal.dig_T1))) >> 12)
                    * ((int32_t)g_bme_cal.dig_T3)) >> 14;
    g_t_fine = var1 + var2;
    return (g_t_fine * 5 + 128) >> 8;  /* temperature in 0.01 °C */
}

static uint32_t bme280_compensate_P(int32_t adc_P) {
    int64_t var1 = ((int64_t)g_t_fine) - 128000;
    int64_t var2 = var1 * var1 * (int64_t)g_bme_cal.dig_P6;
    var2 = var2 + ((var1 * (int64_t)g_bme_cal.dig_P5) << 17);
    var2 = var2 + (((int64_t)g_bme_cal.dig_P4) << 35);
    var1 = ((var1 * var1 * (int64_t)g_bme_cal.dig_P3) >> 8)
         + ((var1 * (int64_t)g_bme_cal.dig_P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)g_bme_cal.dig_P1) >> 33;
    if (var1 == 0) return 0;
    int64_t p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)g_bme_cal.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)g_bme_cal.dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)g_bme_cal.dig_P7) << 4);
    return (uint32_t)p;
}

static uint32_t bme280_compensate_H(int32_t adc_H) {
    int32_t v_x1_u32r = (g_t_fine - ((int32_t)76800));
    v_x1_u32r = (((((adc_H << 14) - (((int32_t)g_bme_cal.dig_H4) << 20)
                    - (((int32_t)g_bme_cal.dig_H5) * v_x1_u32r)) + ((int32_t)16384)) >> 15)
                 * (((((((v_x1_u32r * ((int32_t)g_bme_cal.dig_H6)) >> 10)
                        * (((v_x1_u32r * ((int32_t)g_bme_cal.dig_H3)) >> 11) + ((int32_t)32768))) >> 10)
                      + ((int32_t)2097152)) * ((int32_t)g_bme_cal.dig_H2) + 8192) >> 14));
    v_x1_u32r = (v_x1_u32r - (((((v_x1_u32r >> 15) * (v_x1_u32r >> 15)) >> 7)
                               * ((int32_t)g_bme_cal.dig_H1)) >> 4));
    v_x1_u32r = (v_x1_u32r < 0) ? 0 : v_x1_u32r;
    v_x1_u32r = (v_x1_u32r > 419430400) ? 419430400 : v_x1_u32r;
    return (uint32_t)(v_x1_u32r >> 12);
}

/* ── BME280 init + read ───────────────────────────────────────── */

static bool bme280_init(void) {
    uint8_t id;
    if (i2c_read_reg(BME280_ADDR, BME280_REG_ID, &id, 1) != ESP_OK) {
        ESP_LOGW(TAG, "BME280 not found");
        return false;
    }
    if (id != BME280_ID_VAL) {
        ESP_LOGW(TAG, "BME280 bad ID: 0x%02X", id);
        return false;
    }

    /* Reset */
    i2c_write_reg(BME280_ADDR, BME280_REG_RESET, BME280_RESET_CMD);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Read calibration data */
    uint8_t calib1[BME280_CALIB1_LEN];
    uint8_t calib2[BME280_CALIB2_LEN];
    i2c_read_reg(BME280_ADDR, BME280_REG_CALIB_1, calib1, BME280_CALIB1_LEN);
    i2c_read_reg(BME280_ADDR, BME280_REG_CALIB_2, calib2, BME280_CALIB2_LEN);

    g_bme_cal.dig_T1 = (uint16_t)(calib1[0] | (calib1[1] << 8));
    g_bme_cal.dig_T2 = (int16_t)(calib1[2] | (calib1[3] << 8));
    g_bme_cal.dig_T3 = (int16_t)(calib1[4] | (calib1[5] << 8));
    g_bme_cal.dig_P1 = (uint16_t)(calib1[6] | (calib1[7] << 8));
    g_bme_cal.dig_P2 = (int16_t)(calib1[8] | (calib1[9] << 8));
    g_bme_cal.dig_P3 = (int16_t)(calib1[10] | (calib1[11] << 8));
    g_bme_cal.dig_P4 = (int16_t)(calib1[12] | (calib1[13] << 8));
    g_bme_cal.dig_P5 = (int16_t)(calib1[14] | (calib1[15] << 8));
    g_bme_cal.dig_P6 = (int16_t)(calib1[16] | (calib1[17] << 8));
    g_bme_cal.dig_P7 = (int16_t)(calib1[18] | (calib1[19] << 8));
    g_bme_cal.dig_P8 = (int16_t)(calib1[20] | (calib1[21] << 8));
    g_bme_cal.dig_P9 = (int16_t)(calib1[22] | (calib1[23] << 8));
    g_bme_cal.dig_H1 = calib1[25];
    g_bme_cal.dig_H2 = (int16_t)(calib2[0] | (calib2[1] << 8));
    g_bme_cal.dig_H3 = calib2[2];
    g_bme_cal.dig_H4 = (int16_t)((calib2[3] << 4) | (calib2[4] & 0x0F));
    g_bme_cal.dig_H5 = (int16_t)((calib2[5] << 4) | ((calib2[4] >> 4) & 0x0F));
    g_bme_cal.dig_H6 = (int8_t)calib2[6];

    /* Configure: T=1x oversampling, P=1x oversampling, H=1x oversampling, forced mode */
    i2c_write_reg(BME280_ADDR, BME280_REG_CTRL_HUM, 0x01);   /* humidity 1x */
    i2c_write_reg(BME280_ADDR, BME280_REG_CTRL_MEAS, 0x25);  /* T=1x P=1x forced mode */
    i2c_write_reg(BME280_ADDR, BME280_REG_CONFIG, 0x00);     /* no standby, no filter */

    ESP_LOGI(TAG, "BME280 initialized");
    return true;
}

static bool bme280_read(float *temp, float *humi, float *pres) {
    /* Trigger measurement */
    i2c_write_reg(BME280_ADDR, BME280_REG_CTRL_MEAS, 0x25);
    vTaskDelay(pdMS_TO_TICKS(10));  /* 1x oversampling = 7.23ms max */

    uint8_t data[8];
    if (i2c_read_reg(BME280_ADDR, BME280_REG_DATA, data, 8) != ESP_OK) {
        return false;
    }

    int32_t adc_P = ((int32_t)data[0] << 12) | ((int32_t)data[1] << 4) | (data[2] >> 4);
    int32_t adc_T = ((int32_t)data[3] << 12) | ((int32_t)data[4] << 4) | (data[5] >> 4);
    int32_t adc_H = ((int32_t)data[6] << 8)  | data[7];

    int32_t t_raw = bme280_compensate_T(adc_T);
    *temp = t_raw / 100.0f;
    *pres = bme280_compensate_P(adc_P) / 25600.0f;  /* Pa → hPa */
    *humi = bme280_compensate_H(adc_H) / 1024.0f;   /* %RH */

    return true;
}

/* ── SGP30 init + read ────────────────────────────────────────── */

static bool sgp30_init(void) {
    /* Init air quality */
    uint8_t cmd[2] = { (SGP30_REG_INIT_AIR_QUALITY >> 8) & 0xFF,
                       SGP30_REG_INIT_AIR_QUALITY & 0xFF };
    esp_err_t err = i2c_master_write_to_device(I2C_PORT, SGP30_ADDR, cmd, 2,
                                               pdMS_TO_TICKS(10));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SGP30 not found");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(15));  /* init takes ~15ms */

    ESP_LOGI(TAG, "SGP30 initialized");
    return true;
}

static bool sgp30_read(uint16_t *co2, uint16_t *tvoc) {
    uint8_t cmd[2] = { (SGP30_REG_MEASURE_AIR_QUALITY >> 8) & 0xFF,
                       SGP30_REG_MEASURE_AIR_QUALITY & 0xFF };
    if (i2c_master_write_to_device(I2C_PORT, SGP30_ADDR, cmd, 2, pdMS_TO_TICKS(10)) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(12));  /* measurement takes 12ms */

    uint8_t data[6];  /* co2(2) + tvoc(2) + crc(1) + crc(1) */
    if (i2c_master_read_from_device(I2C_PORT, SGP30_ADDR, data, 6, pdMS_TO_TICKS(10)) != ESP_OK) {
        return false;
    }

    *co2  = ((uint16_t)data[0] << 8) | data[1];
    *tvoc = ((uint16_t)data[3] << 8) | data[4];

    return true;
}

/* ── BH1750 init + read ───────────────────────────────────────── */

static bool bh1750_init(void) {
    /* Power on */
    esp_err_t err = i2c_write_reg(BH1750_ADDR, BH1750_POWER_ON, 0);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Set continuous high-res mode */
    err = i2c_write_reg(BH1750_ADDR, BH1750_CONT_HRES_MODE, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BH1750 not found");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(180));  /* first measurement takes ~180ms */

    ESP_LOGI(TAG, "BH1750 initialized");
    return true;
}

static bool bh1750_read(float *lux) {
    uint8_t data[2];
    if (i2c_read_reg(BH1750_ADDR, 0x00 /* no register, direct read */, data, 2) != ESP_OK) {
        /* Actually BH1750 has no register addressing — try raw read */
        if (i2c_master_read_from_device(I2C_PORT, BH1750_ADDR, data, 2, pdMS_TO_TICKS(10)) != ESP_OK) {
            return false;
        }
    }
    uint16_t raw = ((uint16_t)data[0] << 8) | data[1];
    *lux = raw / 1.2f;
    return true;
}

/* ── Public API ───────────────────────────────────────────────── */

void env_sensor_init(void) {
    ESP_LOGI(TAG, "initializing environmental sensors");

    g_bme280_ok = bme280_init();
    g_sgp30_ok  = sgp30_init();
    g_bh1750_ok = bh1750_init();

    g_env_data = (env_data_t){0};

    ESP_LOGI(TAG, "env sensors: BME280=%s SGP30=%s BH1750=%s",
             g_bme280_ok ? "OK" : "N/A",
             g_sgp30_ok  ? "OK" : "N/A",
             g_bh1750_ok ? "OK" : "N/A");
}

bool env_sensor_get_latest(env_data_t *out) {
    if (!out) return false;

    portENTER_CRITICAL(&g_spinlock);
    *out = g_env_data;
    portEXIT_CRITICAL(&g_spinlock);

    return true;
}

void task_env_sensor(void *arg) {
    ESP_LOGI(TAG, "env sensor task started (1Hz)");

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(1000);  /* 1Hz */

    while (1) {
        env_data_t reading = {0};

        if (g_bme280_ok) {
            bme280_read(&reading.temperature, &reading.humidity, &reading.pressure);
        }
        if (g_sgp30_ok) {
            sgp30_read(&reading.co2_ppm, &reading.tvoc_ppb);
        }
        if (g_bh1750_ok) {
            bh1750_read(&reading.ambient_light);
        }

        reading.timestamp_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

        portENTER_CRITICAL(&g_spinlock);
        g_env_data = reading;
        portEXIT_CRITICAL(&g_spinlock);

        ESP_LOGD(TAG, "T=%.1fC H=%.1f%% P=%.1fhPa CO2=%dppm L=%.1flux",
                 reading.temperature, reading.humidity, reading.pressure,
                 reading.co2_ppm, reading.ambient_light);

        vTaskDelayUntil(&last_wake, period);
        esp_task_wdt_reset();
    }
}
