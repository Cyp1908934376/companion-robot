/**
 * Servo driver — head pan/tilt control.
 *
 * Two SG90 micro servos:
 *   - Pan:  -90deg to +90deg (PWM 500–2500us)
 *   - Tilt: -45deg to +45deg (PWM 500–2500us)
 *
 * PWM at 50Hz. Software ramp for smooth motion.
 */

#ifndef SERVO_H
#define SERVO_H

#include "freertos/FreeRTOS.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Servo identifiers. */
typedef enum {
    SERVO_PAN = 0,
    SERVO_TILT,
    SERVO_COUNT,
} servo_id_t;

/** Servo angle command. */
typedef struct {
    servo_id_t id;
    int16_t    angle_deg;     /* target angle in degrees */
    uint16_t   speed_dps;     /* degrees/sec (0 = instant) */
} servo_cmd_t;

/** Initialize servo PWM channels. */
void servo_init(void);

/** Set servo angle with optional ramping. */
void servo_set_angle(servo_id_t id, int16_t angle_deg, uint16_t speed_dps);

/** Servo control task (Core 1). */
void task_servo_ctrl(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* SERVO_H */
