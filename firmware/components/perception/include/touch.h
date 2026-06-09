/**
 * Capacitive touch sensor — ESP32-S3 built-in touch pads.
 *
 * Four touch zones for physical interaction:
 *   - Head (petting)
 *   - Back (carrying)
 *   - Left side (nudge left)
 *   - Right side (nudge right)
 */

#ifndef TOUCH_H
#define TOUCH_H

#include "freertos/FreeRTOS.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Touch zone identifiers. */
typedef enum {
    TOUCH_ZONE_HEAD = 0,
    TOUCH_ZONE_BACK,
    TOUCH_ZONE_LEFT,
    TOUCH_ZONE_RIGHT,
    TOUCH_ZONE_COUNT,
} touch_zone_t;

/** Touch event. */
typedef struct {
    touch_zone_t zone;
    bool        touched;
    uint32_t    duration_ms;  /* how long held (0 = release) */
    uint32_t    timestamp_ms;
} touch_event_t;

/** Initialize capacitive touch pads. */
void touch_init(void);

/** Touch polling task (Core 1, 50Hz). */
void task_touch(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* TOUCH_H */
