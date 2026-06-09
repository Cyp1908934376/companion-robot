/**
 * Connection Manager — automatic mode switching between WiFi, BLE, and offline.
 *
 * Priority: WiFi direct > BLE bridge > offline fallback
 * - WiFi drops 5s  → attempt BLE
 * - BLE drops 10s  → offline mode
 * - WiFi recovers   → immediate switch back
 */

#ifndef CONN_MANAGER_H
#define CONN_MANAGER_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Connection states */
typedef enum {
    CONN_STATE_INIT,
    CONN_STATE_WIFI_CONNECTING,
    CONN_STATE_WIFI_CONNECTED,
    CONN_STATE_WS_CONNECTING,
    CONN_STATE_WS_CONNECTED,      /* Direct mode */
    CONN_STATE_BLE_ADVERTISING,
    CONN_STATE_BLE_CONNECTED,     /* Bridge mode */
    CONN_STATE_OFFLINE,           /* Fallback mode */
    CONN_STATE_ERROR,
} conn_state_t;

/* Connection manager context */
typedef struct {
    conn_state_t state;
    uint32_t     last_wifi_ok_ms;
    uint32_t     last_ble_ok_ms;
    uint8_t      reconnect_attempt;
} conn_mgr_t;

extern conn_mgr_t g_conn_mgr;
extern EventGroupHandle_t evg_system;

/* Event bits */
#define EVG_WIFI_CONNECTED     BIT0
#define EVG_WS_CONNECTED       BIT1
#define EVG_BLE_CONNECTED      BIT2
#define EVG_SENSOR_READY       BIT3
#define EVG_EMERGENCY          BIT4
#define EVG_LOW_BATTERY        BIT5

/* ── Delta heartbeat types ─────────────────────────────────── */

/** Heartbeat encoding type. Header byte encodes type in bits 7:6. */
typedef enum {
    HB_NO_CHANGE     = 0x00,  /* 00xxxxxx: no state change → 3B total */
    HB_BATTERY_ONLY  = 0x40,  /* 01xxxxxx: only battery changed → 4B total */
    HB_STATUS_CHANGE = 0x80,  /* 10xxxxxx: status flags changed → 5B total */
    HB_FULL          = 0xC0,  /* 11xxxxxx: full heartbeat → 8B total */
} hb_type_t;

/** Cached heartbeat state for delta encoding. */
typedef struct {
    uint8_t  status;
    uint8_t  battery;
    int8_t   rssi;
    uint16_t task_id;
    uint8_t  full_countdown;  /* send full heartbeat every N intervals */
} hb_cache_t;

/** Encode and transmit a delta heartbeat frame.
 *  Returns number of bytes transmitted (0 on error). */
size_t conn_send_heartbeat(uint16_t short_id, uint8_t status, uint8_t battery,
                           int8_t rssi, uint16_t task_id);

/** Main connection manager task (runs on Core 0). */
void task_conn_manager(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* CONN_MANAGER_H */
