/**
 * Camera driver — OV2640 over SPI.
 *
 * Captures JPEG frames at up to 15fps (UXGA).
 * Used for: face detection, object recognition, obstacle classification.
 */

#ifndef CAMERA_H
#define CAMERA_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Camera frame info (queued to vision task). */
typedef struct {
    uint8_t *data;
    size_t   len;
    uint32_t timestamp_ms;
} camera_frame_t;

/** Initialize OV2640 over SPI. */
void camera_init(void);

/** Vision processing task (Core 1). */
void task_vision(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_H */
