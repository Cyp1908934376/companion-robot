/**
 * Charging station interface.
 *
 * Detects docked state via IR proximity + charging voltage sense.
 * Communicates with charging station MCU over one-wire UART
 * for charge current negotiation and temperature monitoring.
 */

#ifndef CHARGING_H
#define CHARGING_H

#include "freertos/FreeRTOS.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Charging status. */
typedef enum {
    CHARGE_NOT_CONNECTED,
    CHARGE_DOCKED_IDLE,        /* docked but not charging (full) */
    CHARGE_CC,                 /* constant current phase */
    CHARGE_CV,                 /* constant voltage phase */
    CHARGE_TRICKLE,            /* maintenance charge */
    CHARGE_FAULT,              /* over-temp or timeout */
} charge_state_t;

/** Initialize charging detector and station UART. */
void charging_init(void);

/** Get current charging state. */
charge_state_t charging_get_state(void);

/** Is the robot currently docked? */
bool charging_is_docked(void);

#ifdef __cplusplus
}
#endif

#endif /* CHARGING_H */
