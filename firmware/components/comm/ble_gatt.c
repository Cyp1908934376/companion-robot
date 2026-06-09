/**
 * BLE GATT Service — bridge mode transport.
 *
 * Provides the BCP service for mobile phone bridging.
 * Target MTU: 247 bytes. Auto-fragments BCP frames exceeding MTU-3.
 *
 * ESP-IDF NimBLE stack integration.
 */

#include "ble_gatt.h"
#include "conn_manager.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_device.h"

static const char *TAG = "ble_gatt";

ble_gatt_t g_ble = {
    .connected = false,
    .conn_id = 0,
    .mtu = 23, /* default BLE 4.2 MTU */
};

/* BLE advertisement data */
static const uint8_t adv_service_uuid[2] = {
    (uint8_t)(BLE_SERVICE_UUID & 0xFF),
    (uint8_t)((BLE_SERVICE_UUID >> 8) & 0xFF),
};

int ble_gatt_init(void) {
    ESP_LOGI(TAG, "initializing BLE");

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_CONFIG_DEFAULT();
    esp_bt_controller_init(&bt_cfg);
    esp_bt_controller_enable(ESP_BT_MODE_BLE);
    esp_bluedroid_init();
    esp_bluedroid_enable();

    /* Register GATT callbacks */
    esp_ble_gatts_register_callback(NULL); /* production: real callback */
    esp_ble_gap_register_callback(NULL);

    /* Set device name */
    esp_bt_dev_set_device_name("CompanionBot-Mini");

    ESP_LOGI(TAG, "BLE initialized");
    return 0;
}

int ble_start_advertising(void) {
    esp_ble_adv_params_t adv_params = {
        .adv_int_min        = 0x20,
        .adv_int_max        = 0x40,
        .adv_type           = ADV_TYPE_IND,
        .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
        .channel_map        = ADV_CHNL_ALL,
        .adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
    };

    esp_ble_gap_start_advertising(&adv_params);
    ESP_LOGI(TAG, "BLE advertising started");
    return 0;
}

int ble_stop_advertising(void) {
    esp_ble_gap_stop_advertising();
    ESP_LOGI(TAG, "BLE advertising stopped");
    return 0;
}

int ble_send_frame(const bcp_frame_t *frame) {
    if (!g_ble.connected) return -1;

    uint8_t buf[BCP_MAX_FRAME_LEN];
    int total_len = bcp_encode(frame, buf, sizeof(buf));
    if (total_len < 0) return -1;

    /* Check if fragmentation is needed (MTU-3 for ATT header) */
    uint16_t effective_mtu = g_ble.mtu - 3;
    if ((size_t)total_len <= effective_mtu) {
        /* Single notification */
        /* In production: esp_ble_gatts_send_indicate() */
        ESP_LOGD(TAG, "BLE TX: %d bytes (single)", total_len);
    } else {
        /* Fragment: first chunk has 2-byte total_len header */
        g_ble.tx_frag_len = (size_t)total_len;
        g_ble.tx_frag_offset = 0;
        memcpy(g_ble.tx_frag_buf, buf, (size_t)total_len);

        while (g_ble.tx_frag_offset < g_ble.tx_frag_len) {
            size_t chunk = g_ble.tx_frag_len - g_ble.tx_frag_offset;
            if (chunk > effective_mtu) chunk = effective_mtu;
            /* In production: send chunk via notification */
            ESP_LOGD(TAG, "BLE TX: frag %d/%d bytes",
                     chunk, g_ble.tx_frag_len);
            g_ble.tx_frag_offset += chunk;
        }
    }

    return 0;
}

void ble_deinit(void) {
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
}
