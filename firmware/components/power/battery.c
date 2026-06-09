/**
 * Battery monitor — MAX17048 fuel gauge over I2C.
 *
 * 1S Li-Po (3.7V nominal, 4.2V full, 3.0V cutoff).
 * Reads SOC (%), voltage (V), and current (mA) at 1Hz.
 */

#include "battery.h"
#include "config.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "freertos/task.h"

static const char *TAG = "battery";

/* ── MAX17048 Register map ────────────────────────────────────── */

#define MAX17048_ADDR           0x36
#define MAX17048_REG_VCELL      0x02   /* voltage, 78.125uV/LSB */
#define MAX17048_REG_SOC        0x04   /* state of charge, 1/256% per LSB */
#define MAX17048_REG_MODE       0x06   /* quick-start */
#define MAX17048_REG_VERSION    0x08   /* chip version */
#define MAX17048_REG_HIBRT      0x0A   /* hibernate config */
#define MAX17048_REG_CONFIG     0x0C   /* config */
#define MAX17048_REG_VALRT      0x14   /* voltage alert */
#define MAX17048_REG_CRATE      0x16   /* charge/discharge rate */
#define MAX17048_REG_VRESET     0x18   /* reset voltage */
#define MAX17048_REG_STATUS     0x1A   /* status */
#define MAX17048_REG_CMD        0xFE   /* command */

#define CMD_POR                 0x5400  /* power-on reset */

/* ── State ────────────────────────────────────────────────────── */

static battery_status_t g_battery;
static bool g_fuel_gauge_ok;

/* ── I2C helpers ──────────────────────────────────────────────── */

static esp_err_t max17048_read(uint8_t reg, uint8_t *data, size_t len) {
    return i2c_master_write_read_device(I2C_PORT, MAX17048_ADDR,
                                        &reg, 1, data, len, pdMS_TO_TICKS(10));
}

static esp_err_t max17048_write(uint8_t reg, uint16_t val) {
    uint8_t buf[3] = { reg, (val >> 8) & 0xFF, val & 0xFF };
    return i2c_master_write_to_device(I2C_PORT, MAX17048_ADDR, buf, 3, pdMS_TO_TICKS(10));
}

/* ── Public API ───────────────────────────────────────────────── */

void battery_init(void) {
    ESP_LOGI(TAG, "initializing MAX17048 fuel gauge (I2C 0x%02X)", MAX17048_ADDR);

    /* Verify chip */
    uint8_t version[2];
    esp_err_t err = max17048_read(MAX17048_REG_VERSION, version, 2);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MAX17048 not found — using fixed SOC=85%%");
        g_fuel_gauge_ok = false;
        g_battery = (battery_status_t){
            .voltage    = 3.85f,
            .current_ma = 0.0f,
            .soc_pct    = 85,
            .low        = false,
            .critical   = false,
        };
        return;
    }

    /* Power-on reset */
    max17048_write(MAX17048_REG_CMD, CMD_POR);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Quick-start: clear to restart fuel gauge algorithm */
    uint8_t mode[2];
    max17048_read(MAX17048_REG_MODE, mode, 2);
    mode[0] &= ~0x40;  /* clear QuickStart bit */
    max17048_write(MAX17048_REG_MODE, ((uint16_t)mode[0] << 8) | mode[1]);

    /* Configure: no hibernation, alert at 4.0V (reset indicator) */
    max17048_write(MAX17048_REG_HIBRT, 0x0000);   /* always active */
    max17048_write(MAX17048_REG_CONFIG, 0x0000);   /* default config */

    g_fuel_gauge_ok = true;

    /* Read initial SOC */
    battery_get_status();

    ESP_LOGI(TAG, "MAX17048 ready — SOC=%d%% V=%.2fV",
             g_battery.soc_pct, g_battery.voltage);
}

battery_status_t battery_get_status(void) {
    if (!g_fuel_gauge_ok) {
        g_battery.timestamp_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        return g_battery;
    }

    /* Read VCELL (2 bytes, 78.125uV/LSB) */
    uint8_t vcell_buf[2];
    if (max17048_read(MAX17048_REG_VCELL, vcell_buf, 2) == ESP_OK) {
        uint16_t vcell = ((uint16_t)vcell_buf[0] << 8) | vcell_buf[1];
        g_battery.voltage = vcell * 78.125e-6f;
    }

    /* Read SOC (2 bytes, 1/256% per LSB) */
    uint8_t soc_buf[2];
    if (max17048_read(MAX17048_REG_SOC, soc_buf, 2) == ESP_OK) {
        uint16_t soc = ((uint16_t)soc_buf[0] << 8) | soc_buf[1];
        g_battery.soc_pct = (uint8_t)(soc >> 8);  /* upper byte = integer % */
    }

    /* Read charge/discharge rate (2 bytes, 0.208%/hr per LSB) */
    uint8_t crate_buf[2];
    if (max17048_read(MAX17048_REG_CRATE, crate_buf, 2) == ESP_OK) {
        int16_t crate = (int16_t)(((uint16_t)crate_buf[0] << 8) | crate_buf[1]);
        /* Convert to mA: 0.208% per hour × battery capacity / 100.
         * Assuming 2000mAh battery: crate_val × 2000 × 0.208 / 100. */
        g_battery.current_ma = crate * 2000.0f * 0.208f / 100.0f;
    }

    /* Update threshold flags */
    g_battery.low      = g_battery.soc_pct < BATTERY_LOW_PCT;
    g_battery.critical = g_battery.soc_pct < BATTERY_CRITICAL_PCT;
    g_battery.timestamp_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    return g_battery;
}

float battery_get_level(void) {
    battery_get_status();
    return g_battery.soc_pct / 100.0f;
}

bool battery_is_low(void) {
    battery_get_status();
    return g_battery.low;
}

bool battery_is_critical(void) {
    battery_get_status();
    return g_battery.critical;
}
