/**
 * OTA (Over-The-Air) firmware update.
 */

#include "ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_flash_partitions.h"

static const char *TAG = "ota";

static ota_state_t g_ota_state = OTA_IDLE;
static esp_ota_handle_t g_ota_handle = 0;
static const esp_partition_t *g_ota_partition = NULL;

void ota_init(void) {
    ESP_LOGI(TAG, "initializing OTA subsystem");

    /* Check for unverified boot from previous OTA */
    g_ota_partition = esp_ota_get_next_update_partition(NULL);
    if (g_ota_partition) {
        ESP_LOGI(TAG, "OTA partition: %s", g_ota_partition->label);
    }
}

ota_state_t ota_get_state(void) {
    return g_ota_state;
}

esp_err_t ota_start(size_t total_size, const uint8_t *expected_hash) {
    ESP_LOGI(TAG, "OTA start: total_size=%d", total_size);

    /* In production:
     *   1. Get next OTA partition
     *   2. esp_ota_begin() to start writing
     *   3. Set state to OTA_RECEIVING
     */
    g_ota_state = OTA_ERASING;
    ESP_LOGW(TAG, "OTA stub — no actual update");
    g_ota_state = OTA_RECEIVING;
    return ESP_OK;
}

esp_err_t ota_write_chunk(uint32_t offset, const uint8_t *data, size_t len) {
    /* In production: esp_ota_write() with offset */
    ESP_LOGD(TAG, "OTA chunk: offset=%lu len=%d", offset, len);
    return ESP_OK;
}

esp_err_t ota_finish(void) {
    ESP_LOGI(TAG, "OTA finish: verifying signature");

    g_ota_state = OTA_VERIFYING;

    /* In production:
     *   1. Read back firmware from partition
     *   2. Verify Ed25519 signature against stored public key
     *   3. esp_ota_end()
     *   4. esp_ota_set_boot_partition()
     */

    g_ota_state = OTA_READY;
    ESP_LOGW(TAG, "OTA stub — reboot skipped");
    return ESP_OK;
}

void ota_abort(void) {
    ESP_LOGW(TAG, "OTA aborted");
    if (g_ota_state == OTA_RECEIVING && g_ota_handle) {
        /* esp_ota_end() with abort */
    }
    g_ota_state = OTA_FAILED;
}

bool ota_check_pending(void) {
    /* In production: check if boot partition was newly swapped,
     * verify boot count, handle rollback */
    return false;
}
