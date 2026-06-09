/**
 * NVS configuration — persistent settings storage.
 */

#include "nvs_config.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "nvs_cfg";

static nvs_handle_t g_nvs_handle;

void nvs_config_init(void) {
    ESP_LOGI(TAG, "initializing NVS config (namespace: %s)", NVS_NAMESPACE);

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &g_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
    }
}

esp_err_t nvs_config_get_str(const char *key, char *out, size_t *len) {
    return nvs_get_str(g_nvs_handle, key, out, len);
}

esp_err_t nvs_config_set_str(const char *key, const char *val) {
    esp_err_t err = nvs_set_str(g_nvs_handle, key, val);
    if (err == ESP_OK) nvs_commit(g_nvs_handle);
    return err;
}

esp_err_t nvs_config_get_u32(const char *key, uint32_t *out) {
    return nvs_get_u32(g_nvs_handle, key, out);
}

esp_err_t nvs_config_set_u32(const char *key, uint32_t val) {
    esp_err_t err = nvs_set_u32(g_nvs_handle, key, val);
    if (err == ESP_OK) nvs_commit(g_nvs_handle);
    return err;
}

esp_err_t nvs_config_get_blob(const char *key, void *out, size_t *len) {
    return nvs_get_blob(g_nvs_handle, key, out, len);
}

esp_err_t nvs_config_set_blob(const char *key, const void *data, size_t len) {
    esp_err_t err = nvs_set_blob(g_nvs_handle, key, data, len);
    if (err == ESP_OK) nvs_commit(g_nvs_handle);
    return err;
}

esp_err_t nvs_config_erase(const char *key) {
    esp_err_t err = nvs_erase_key(g_nvs_handle, key);
    if (err == ESP_OK) nvs_commit(g_nvs_handle);
    return err;
}

esp_err_t nvs_config_commit(void) {
    return nvs_commit(g_nvs_handle);
}
