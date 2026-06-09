/**
 * OTA (Over-The-Air) firmware update.
 *
 * Dual-partition scheme: factory + ota_0 + ota_1.
 *   - Update via WiFi (WebSocket binary stream)
 *   - Update via BLE (chunked OTA characteristic)
 *   - Ed25519 signature verification
 *   - Rollback protection (boot-count tracking)
 *
 * Process:
 *   1. Receive OTA_START command (total_size, chunk_size, firmware_hash)
 *   2. Erase target partition
 *   3. Receive OTA_CHUNK commands, write to partition
 *   4. Receive OTA_DONE command, verify signature
 *   5. Set boot partition and reboot
 */

#ifndef OTA_H
#define OTA_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** OTA update state. */
typedef enum {
    OTA_IDLE,
    OTA_ERASING,
    OTA_RECEIVING,
    OTA_VERIFYING,
    OTA_READY,      /* ready to reboot */
    OTA_FAILED,
} ota_state_t;

/** Initialize OTA subsystem. */
void ota_init(void);

/** Get current OTA state. */
ota_state_t ota_get_state(void);

/** Start a new OTA update. Erases target partition. */
esp_err_t ota_start(size_t total_size, const uint8_t *expected_hash);

/** Write a chunk of firmware data. */
esp_err_t ota_write_chunk(uint32_t offset, const uint8_t *data, size_t len);

/** Finish OTA: verify signature and mark for boot. */
esp_err_t ota_finish(void);

/** Abort current OTA update. */
void ota_abort(void);

/** Check for pending update (called at boot). */
bool ota_check_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_H */
