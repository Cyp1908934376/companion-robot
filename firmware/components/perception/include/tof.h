/**
 * ToF distance sensors — VL53L0X x3 over I2C (XSHUT-muxed).
 *
 * Three sensors provide front/side obstacle detection:
 *   - Sensor 1: front (straight ahead)
 *   - Sensor 2: front-left (-45 deg)
 *   - Sensor 3: front-right (+45 deg)
 *
 * Range: 30mm–2000mm. Read at 50Hz per sensor.
 */

#ifndef TOF_H
#define TOF_H

#include "freertos/FreeRTOS.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TOF_COUNT 3

/** Readings from all three ToF sensors. */
typedef struct {
    uint16_t distance_mm[TOF_COUNT];  /* 0 = out of range */
    uint32_t timestamp_ms;
} tof_reading_t;

/** Initialize VL53L0X sensors (address init + XSHUT sequencing). */
void tof_init(void);

/** Read all ToF sensors. Returns false on I2C error. */
bool tof_read_all(tof_reading_t *out);

#ifdef __cplusplus
}
#endif

#endif /* TOF_H */
