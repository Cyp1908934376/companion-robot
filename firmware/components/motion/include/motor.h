/**
 * Motor driver — DRV8833 x2 driving 4x N20 micrometal gearmotors.
 *
 * Mecanum wheel kinematics:
 *   - Forward:  all wheels forward
 *   - Backward: all wheels reverse
 *   - Strafe L: L1 reverse, L2 forward, R1 forward, R2 reverse
 *   - Strafe R: L1 forward, L2 reverse, R1 reverse, R2 forward
 *   - Rotate L: L-side reverse, R-side forward
 *   - Rotate R: L-side forward, R-side reverse
 *
 * PID control loop at 100Hz. PWM resolution 10-bit.
 */

#ifndef MOTOR_H
#define MOTOR_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "bcp_codec.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Motor identifiers. */
typedef enum {
    MOTOR_L1 = 0,  /* front-left */
    MOTOR_L2,      /* rear-left */
    MOTOR_R1,      /* front-right */
    MOTOR_R2,      /* rear-right */
    MOTOR_COUNT,
} motor_id_t;

/** Motor command types. */
typedef enum {
    MOTOR_CMD_MOVE = 0,   /* direction + speed */
    MOTOR_CMD_MOVE_TO,    /* target position (x, y) + speed */
    MOTOR_CMD_STOP,
} motor_cmd_type_t;

/** Motor command (from incoming queue). */
typedef struct {
    motor_cmd_type_t type;
    union {
        struct {
            bcp_direction_t direction;
            uint8_t         speed;       /* 0–255 */
        } move;
        struct {
            int16_t x_cm;
            int16_t y_cm;
            uint8_t speed;               /* 0–255 */
        } move_to;
    };
    uint32_t        duration_ms; /* 0 = continuous */
    bool            no_ack;
} motor_cmd_t;

/** Initialize DRV8833 motor drivers and PWM channels. */
void motor_init(void);

/** Command the robot to drive to a target position (x_cm, y_cm). */
void motor_move_to(int16_t x_cm, int16_t y_cm, uint8_t speed);

/** Cancel the active move-to target. */
void motor_cancel_move_to(void);

/** Check if a move-to operation is in progress. */
bool motor_move_to_active(void);

/** Motor control task (Core 1, 100Hz PID loop). */
void task_motor_ctrl(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* MOTOR_H */
