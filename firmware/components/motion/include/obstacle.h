/**
 * Obstacle detection — fusion of ToF + IMU + camera depth.
 *
 * Provides:
 *   - Nearest obstacle distance per direction
 *   - Emergency stop trigger
 *   - Slow-down zone warnings
 *
 * Thresholds defined in config.h:
 *   OBSTACLE_SAFE_CM (50), OBSTACLE_SLOW_CM (30), OBSTACLE_STOP_CM (10)
 */

#ifndef OBSTACLE_H
#define OBSTACLE_H

#include "freertos/FreeRTOS.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Obstacle direction. */
typedef enum {
    OBS_FRONT = 0,
    OBS_FRONT_LEFT,
    OBS_FRONT_RIGHT,
    OBS_REAR,
    OBS_DIR_COUNT,
} obs_direction_t;

/** Obstacle status. */
typedef struct {
    uint16_t distance_cm[OBS_DIR_COUNT];
    bool     emergency_stop;       /* any direction < OBSTACLE_STOP_CM */
    bool     slow_down;            /* any direction < OBSTACLE_SLOW_CM */
    uint32_t timestamp_ms;
} obstacle_status_t;

/** Initialize obstacle detection (depends on ToF). */
void obstacle_init(void);

/** Get current obstacle status. */
obstacle_status_t obstacle_check(void);

#ifdef __cplusplus
}
#endif

#endif /* OBSTACLE_H */
