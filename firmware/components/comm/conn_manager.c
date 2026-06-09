/**
 * Connection Manager — automatic mode switching.
 *
 * State machine:
 *   INIT → WIFI_CONNECTING → WIFI_CONNECTED → WS_CONNECTING → WS_CONNECTED
 *   Any WiFi failure (5s) → BLE_ADVERTISING → BLE_CONNECTED
 *   BLE failure (10s) → OFFLINE
 *   WiFi recovery → immediate WS_CONNECTED
 */

#include "conn_manager.h"
#include "ws_client.h"
#include "ble_gatt.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "conn_mgr";

conn_mgr_t g_conn_mgr = {
    .state = CONN_STATE_INIT,
    .reconnect_attempt = 0,
};

/* ── Delta heartbeat state ────────────────────────────────────── */

static hb_cache_t g_hb_cache;
static uint8_t    g_hb_seq;  /* sequence counter (0-63) */

/* WiFi event handler */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            xEventGroupClearBits(evg_system, EVG_WIFI_CONNECTED);
            xEventGroupClearBits(evg_system, EVG_WS_CONNECTED);
            ESP_LOGW(TAG, "WiFi disconnected");
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(evg_system, EVG_WIFI_CONNECTED);
        ESP_LOGI(TAG, "WiFi connected, got IP");
    }
}

/* Initialize WiFi station */
static void wifi_init_sta(void) {
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                &wifi_event_handler, NULL);

    wifi_config_t wifi_cfg = { 0 };
    /* Credentials loaded from NVS or set via provisioning */
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_start();
}

/* ── Delta heartbeat encoding ─────────────────────────────────── */

#define HB_FULL_INTERVAL  10   /* send full heartbeat every N intervals */

size_t conn_send_heartbeat(uint16_t short_id, uint8_t status, uint8_t battery,
                           int8_t rssi, uint16_t task_id) {
    uint8_t buf[8];
    size_t len;

    bool battery_changed = (battery != g_hb_cache.battery);
    bool status_changed  = (status != g_hb_cache.status);
    bool any_change      = battery_changed || status_changed
                        || (rssi != g_hb_cache.rssi)
                        || (task_id != g_hb_cache.task_id);

    /* Force full heartbeat every HB_FULL_INTERVAL for sync */
    g_hb_cache.full_countdown--;
    bool force_full = (g_hb_cache.full_countdown == 0);

    hb_type_t type;
    if (force_full) {
        type = HB_FULL;
        g_hb_cache.full_countdown = HB_FULL_INTERVAL;
    } else if (!any_change) {
        type = HB_NO_CHANGE;
    } else if (battery_changed && !status_changed
               && rssi == g_hb_cache.rssi && task_id == g_hb_cache.task_id) {
        type = HB_BATTERY_ONLY;
    } else {
        type = HB_STATUS_CHANGE;
    }

    /* Encode header byte: type (bits 7:6) + sequence (bits 5:0) */
    buf[0] = (uint8_t)type | (g_hb_seq & 0x3F);
    g_hb_seq = (g_hb_seq + 1) & 0x3F;

    /* Encode short_id (2 bytes, little-endian) */
    buf[1] = (uint8_t)(short_id & 0xFF);
    buf[2] = (uint8_t)((short_id >> 8) & 0xFF);
    len = 3;

    switch (type) {
    case HB_NO_CHANGE:
        /* Already done — just header + short_id = 3B */
        break;

    case HB_BATTERY_ONLY:
        buf[3] = battery;
        len = 4;
        break;

    case HB_STATUS_CHANGE:
        buf[3] = status;
        buf[4] = battery;
        len = 5;
        break;

    case HB_FULL:
        buf[3] = status;
        buf[4] = battery;
        buf[5] = (uint8_t)rssi;
        buf[6] = (uint8_t)(task_id & 0xFF);
        buf[7] = (uint8_t)((task_id >> 8) & 0xFF);
        len = 8;
        break;
    }

    /* Update cache */
    g_hb_cache.status   = status;
    g_hb_cache.battery  = battery;
    g_hb_cache.rssi     = rssi;
    g_hb_cache.task_id  = task_id;

    /* Transmit via WebSocket or BLE (actual TX handled by comm_tx task)
     * For now, encode and log. In production: queue to comm_tx via q_cmd_outgoing. */
    ESP_LOGD(TAG, "heartbeat: type=%d seq=%d len=%d (id=%d status=0x%02X batt=%d%%)",
             (type >> 6) & 3, g_hb_seq, len, short_id, status, battery);

    /* TODO: send buf[0..len-1] via WebSocket or BLE
     *   if (ws_ok) ws_send_frame(buf, len);
     *   else if (ble_ok) ble_send(buf, len);
     */
    return len;
}

void task_conn_manager(void *arg) {
    ESP_LOGI(TAG, "connection manager starting");

    /* Initialize subsystems */
    wifi_init_sta();
    ble_gatt_init();

    TickType_t last_wake = xTaskGetTickCount();
    uint32_t wifi_lost_at = 0;
    uint32_t ble_lost_at = 0;
    bool wifi_was_connected = false;

    while (1) {
        EventBits_t bits = xEventGroupGetBits(evg_system);
        bool wifi_ok = (bits & EVG_WIFI_CONNECTED) != 0;
        bool ws_ok   = (bits & EVG_WS_CONNECTED) != 0;
        bool ble_ok  = (bits & EVG_BLE_CONNECTED) != 0;

        uint32_t now = xTaskGetTickCount();

        switch (g_conn_mgr.state) {

        case CONN_STATE_INIT:
            if (wifi_ok) {
                g_conn_mgr.state = CONN_STATE_WIFI_CONNECTED;
                ESP_LOGI(TAG, "state: INIT → WIFI_CONNECTED");
            } else {
                g_conn_mgr.state = CONN_STATE_WIFI_CONNECTING;
                ESP_LOGI(TAG, "state: INIT → WIFI_CONNECTING");
            }
            break;

        case CONN_STATE_WIFI_CONNECTING:
            if (wifi_ok) {
                g_conn_mgr.state = CONN_STATE_WIFI_CONNECTED;
                ESP_LOGI(TAG, "state: WIFI_CONNECTING → WIFI_CONNECTED");
            } else if (now > WIFI_CONNECT_TIMEOUT_MS) {
                /* WiFi failed — try BLE bridge */
                ble_start_advertising();
                g_conn_mgr.state = CONN_STATE_BLE_ADVERTISING;
                ESP_LOGW(TAG, "WiFi timeout, switching to BLE");
            }
            break;

        case CONN_STATE_WIFI_CONNECTED:
            /* Attempt WebSocket connection */
            ws_connect("gateway.local", 8080);
            g_conn_mgr.state = CONN_STATE_WS_CONNECTING;
            break;

        case CONN_STATE_WS_CONNECTING:
            if (ws_ok) {
                g_conn_mgr.state = CONN_STATE_WS_CONNECTED;
                g_conn_mgr.reconnect_attempt = 0;
                ESP_LOGI(TAG, "state: WS_CONNECTING → WS_CONNECTED (direct mode)");
            } else if (!wifi_ok) {
                ws_disconnect();
                g_conn_mgr.state = CONN_STATE_WIFI_CONNECTING;
            }
            break;

        case CONN_STATE_WS_CONNECTED:
            wifi_was_connected = true;
            if (!ws_ok) {
                if (wifi_lost_at == 0) wifi_lost_at = now;
                /* 5 seconds grace period */
                if (now - wifi_lost_at > pdMS_TO_TICKS(5000)) {
                    ESP_LOGW(TAG, "WS lost for 5s, switching to BLE bridge");
                    ble_start_advertising();
                    g_conn_mgr.state = CONN_STATE_BLE_ADVERTISING;
                    wifi_lost_at = 0;
                }
            } else {
                wifi_lost_at = 0;
            }
            break;

        case CONN_STATE_BLE_ADVERTISING:
            if (wifi_ok && wifi_was_connected) {
                /* WiFi recovered — switch back */
                ble_stop_advertising();
                g_conn_mgr.state = CONN_STATE_WIFI_CONNECTED;
                ESP_LOGI(TAG, "WiFi recovered, switching back to direct mode");
            } else if (ble_ok) {
                g_conn_mgr.state = CONN_STATE_BLE_CONNECTED;
                ble_lost_at = 0;
                ESP_LOGI(TAG, "state: BLE_ADVERTISING → BLE_CONNECTED (bridge mode)");
            }
            break;

        case CONN_STATE_BLE_CONNECTED:
            if (wifi_ok && wifi_was_connected) {
                /* WiFi recovered */
                g_conn_mgr.state = CONN_STATE_WIFI_CONNECTED;
                ESP_LOGI(TAG, "WiFi recovered, switching back to direct");
            } else if (!ble_ok) {
                if (ble_lost_at == 0) ble_lost_at = now;
                if (now - ble_lost_at > pdMS_TO_TICKS(10000)) {
                    ESP_LOGW(TAG, "BLE lost for 10s, entering offline mode");
                    g_conn_mgr.state = CONN_STATE_OFFLINE;
                    ble_lost_at = 0;
                }
            } else {
                ble_lost_at = 0;
            }
            break;

        case CONN_STATE_OFFLINE:
            if (wifi_ok && wifi_was_connected) {
                g_conn_mgr.state = CONN_STATE_WIFI_CONNECTED;
                ESP_LOGI(TAG, "recovered from offline");
            } else if (ble_ok) {
                g_conn_mgr.state = CONN_STATE_BLE_CONNECTED;
                ESP_LOGI(TAG, "BLE recovered from offline");
            }
            break;

        case CONN_STATE_ERROR:
        default:
            break;
        }

        /* ── Periodic heartbeat ────────────────────────────────── */
        {
            static uint32_t last_hb_ms = 0;
            uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (now_ms - last_hb_ms >= HEARTBEAT_INTERVAL_MS) {
                last_hb_ms = now_ms;

                /* Only send heartbeat when connected */
                if (g_conn_mgr.state == CONN_STATE_WS_CONNECTED
                    || g_conn_mgr.state == CONN_STATE_BLE_CONNECTED) {

                    /* Gather current state (from sensors via extern globals) */
                    uint8_t status = 0;
                    uint8_t battery_pct = 85;  /* placeholder — use battery_get_status() */
                    int8_t  rssi = -40;
                    uint16_t task_id = 0;

                    /* Read battery from fuel gauge if available */
                    /* battery_status_t bat = battery_get_status(); battery_pct = bat.soc_pct; */

                    /* Read WiFi RSSI */
                    /* esp_wifi_sta_get_ap_info(&ap_info); rssi = ap_info.rssi; */

                    conn_send_heartbeat(0 /* short_id from reg_ack */,
                                        status, battery_pct, rssi, task_id);
                }
            }
        }

        /* Feed watchdog */
        esp_task_wdt_reset();
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(1000));
    }
}
