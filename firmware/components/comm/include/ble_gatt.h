/**
 * BLE GATT service — bridge mode connection via mobile phone.
 *
 * Services:
 *   UUID 0xCB00: BCP Service
 *     - 0xCB01: BCP_TX (Write) — mobile → robot BCP frames
 *     - 0xCB02: BCP_RX (Notify) — robot → mobile BCP frames
 *     - 0xCB03: BCP_CONTROL (Read/Write) — connection parameters
 *     - 0xCB04: DEVICE_INFO (Read) — robot info
 *     - 0xCB05: OTA_DATA (Write) — OTA firmware updates
 *
 * Target MTU: 247 bytes. Auto-fragments for smaller MTUs.
 */

#ifndef BLE_GATT_H
#define BLE_GATT_H

#include "freertos/FreeRTOS.h"
#include "bcp_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_SERVICE_UUID        0xCB00
#define BLE_CHAR_BCP_TX         0xCB01
#define BLE_CHAR_BCP_RX         0xCB02
#define BLE_CHAR_BCP_CONTROL    0xCB03
#define BLE_CHAR_DEVICE_INFO    0xCB04
#define BLE_CHAR_OTA_DATA       0xCB05

#define BLE_TARGET_MTU          247
#define BLE_ADV_INTERVAL_MS     100

/** BLE GATT context. */
typedef struct {
    bool     connected;
    uint16_t conn_id;
    uint16_t mtu;
    uint8_t  tx_frag_buf[BCP_MAX_FRAME_LEN];
    size_t   tx_frag_len;
    size_t   tx_frag_offset;
} ble_gatt_t;

extern ble_gatt_t g_ble;

/** Initialize BLE stack and GATT services. */
int ble_gatt_init(void);

/** Start BLE advertising. */
int ble_start_advertising(void);

/** Stop BLE advertising. */
int ble_stop_advertising(void);

/** Send a BCP frame over BLE (auto-fragmented if needed). */
int ble_send_frame(const bcp_frame_t *frame);

/** Deinitialize BLE. */
void ble_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_GATT_H */
