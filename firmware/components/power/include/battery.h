/**
 * Battery monitor — MAX17048 fuel gauge over I2C.
 *
 * 1S Li-Po (3.7V nominal, 4.2V full, 3.0V cutoff).
 * Reads SOC (state-of-charge %), voltage, and current at 1Hz.
 */

#ifndef BATTERY_H
#define BATTERY_H

#include "freertos/FreeRTOS.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Battery status snapshot. */
typedef struct {
    float    voltage;      /* V */
    float    current_ma;   /* mA (positive = discharging, negative = charging) */
    uint8_t  soc_pct;      /* state of charge % */
    bool     low;          /* SOC < BATTERY_LOW_PCT */
    bool     critical;     /* SOC < BATTERY_CRITICAL_PCT */
    uint32_t timestamp_ms;
} battery_status_t;

/** Initialize MAX17048 fuel gauge. */
void battery_init(void);

/** Get latest battery reading. */
battery_status_t battery_get_status(void);

/** Convenience: battery level as 0.0–1.0. */
float battery_get_level(void);

/** Convenience: below low threshold? */
bool battery_is_low(void);

/** Convenience: below critical threshold? */
bool battery_is_critical(void);

#ifdef __cplusplus
}
#endif

#endif /* BATTERY_H */
