/**
 * Power manager — coordinates battery, charging, and power states.
 *
 * Power states:
 *   - NORMAL:       full operation
 *   - LOW_BATTERY:  disable camera, reduce motor speed
 *   - CRITICAL:     motors off, only comm + LEDs, seek charger
 *   - CHARGING:     all systems available, don't drive motors
 *   - CHARGED:      trickle charge, systems idle
 */

#ifndef POWER_MGR_H
#define POWER_MGR_H

#include "freertos/FreeRTOS.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Power states. */
typedef enum {
    POWER_NORMAL,
    POWER_LOW_BATTERY,
    POWER_CRITICAL,
    POWER_CHARGING,
    POWER_CHARGED,
    POWER_FAULT,
} power_state_t;

/** Initialize power manager. */
void power_mgr_init(void);

/** Get current power state. */
power_state_t power_mgr_get_state(void);

/** Report user/motor activity — resets idle timer to prevent sleep. */
void power_mgr_report_activity(void);

/** Power management task (Core 1, 2Hz). */
void task_power_mgr(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* POWER_MGR_H */
