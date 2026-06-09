/**
 * Offline mode — local autonomy when disconnected from main brain.
 *
 * Capabilities without network:
 *   - Basic obstacle avoidance (ToF + IMU only)
 *   - Pre-loaded voice commands (~50 keywords, local NLU)
 *   - Pre-loaded facial expressions + LED patterns
 *   - Touch gesture responses
 *   - Environment monitoring + logging
 *
 * Switches to offline mode when:
 *   - conn_manager enters CONN_STATE_OFFLINE
 *   - Explicit OFFLINE command received
 */

#ifndef OFFLINE_H
#define OFFLINE_H

#include "freertos/FreeRTOS.h"
#include "bcp_codec.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize offline mode subsystem. */
void offline_init(void);

/** Check if currently in offline mode. */
bool offline_is_active(void);

/** Process a command locally (no network). */
int offline_process_command(const bcp_command_t *cmd);

/** Offline autonomy task (Core 1, 20Hz). */
void task_offline(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* OFFLINE_H */
