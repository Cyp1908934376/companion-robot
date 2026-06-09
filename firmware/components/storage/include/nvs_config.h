/**
 * NVS configuration — persistent settings storage.
 *
 * Stores in NVS partition:
 *   - WiFi SSID + password
 *   - Robot name
 *   - Calibration data (IMU offsets, motor trim)
 *   - Behavior preferences
 *   - OTA update state
 */

#ifndef NVS_CONFIG_H
#define NVS_CONFIG_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** NVS namespace for robot config. */
#define NVS_NAMESPACE "robot_cfg"

/** Initialize NVS config subsystem. */
void nvs_config_init(void);

/** Read string value. Returns ESP_OK on success. */
esp_err_t nvs_config_get_str(const char *key, char *out, size_t *len);

/** Write string value. */
esp_err_t nvs_config_set_str(const char *key, const char *val);

/** Read uint32 value. */
esp_err_t nvs_config_get_u32(const char *key, uint32_t *out);

/** Write uint32 value. */
esp_err_t nvs_config_set_u32(const char *key, uint32_t val);

/** Read blob value. */
esp_err_t nvs_config_get_blob(const char *key, void *out, size_t *len);

/** Write blob value. */
esp_err_t nvs_config_set_blob(const char *key, const void *data, size_t len);

/** Erase a key. */
esp_err_t nvs_config_erase(const char *key);

/** Commit all pending writes. */
esp_err_t nvs_config_commit(void);

#ifdef __cplusplus
}
#endif

#endif /* NVS_CONFIG_H */
