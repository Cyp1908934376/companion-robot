/**
 * Behavior engine — high-level robot personality and autonomy.
 *
 * Coordinates perception → action pipeline:
 *   1. Fuse sensor inputs (vision, audio, touch, IMU, env)
 *   2. Determine behavior state (idle, exploring, interacting, charging, error)
 *   3. Generate motor/expression/audio commands
 *
 * Behavior states:
 *   - IDLE:       standing by, occasional idle animations
 *   - EXPLORING:  autonomous navigation, obstacle avoidance
 *   - INTERACTING: responding to user (touch, voice, face)
 *   - CHARGING:   seeking or docked at charger
 *   - PLAYING:    executing a user-requested behavior
 *   - ALERT:      detected anomaly (sound, motion, environment)
 */

#ifndef BEHAVIOR_H
#define BEHAVIOR_H

#include "freertos/FreeRTOS.h"
#include "bcp_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Behavior states. */
typedef enum {
    BEHAVIOR_IDLE,
    BEHAVIOR_EXPLORING,
    BEHAVIOR_INTERACTING,
    BEHAVIOR_CHARGING,
    BEHAVIOR_PLAYING,
    BEHAVIOR_ALERT,
    BEHAVIOR_ERROR,
} behavior_state_t;

/** Behavior engine task (Core 1, 50Hz). */
void task_behavior(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* BEHAVIOR_H */
