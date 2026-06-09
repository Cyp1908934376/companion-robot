/**
 * Environmental sensors — BME280 + SGP30 + BH1750 over I2C.
 *
 * Reads at 1Hz:
 *   - Temperature, humidity, pressure (BME280)
 *   - CO2 equivalent, TVOC (SGP30)
 *   - Ambient light lux (BH1750)
 */

#ifndef ENV_SENSOR_H
#define ENV_SENSOR_H

#include "freertos/FreeRTOS.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Combined environmental reading. */
typedef struct {
    float temperature;     /* Celsius */
    float humidity;        /* %RH */
    float pressure;        /* hPa */
    uint16_t co2_ppm;      /* CO2 equivalent ppm */
    uint16_t tvoc_ppb;     /* TVOC ppb */
    float ambient_light;   /* lux */
    uint32_t timestamp_ms;
} env_data_t;

/** Initialize BME280, SGP30, BH1750. */
void env_sensor_init(void);

/** Get latest reading. Returns false if sensors not ready. */
bool env_sensor_get_latest(env_data_t *out);

/** Environment sensor polling task (Core 1, 1Hz). */
void task_env_sensor(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* ENV_SENSOR_H */
