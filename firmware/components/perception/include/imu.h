/**
 * IMU driver — BMI270 6-axis over I2C.
 *
 * Provides:
 *   - 200Hz accelerometer + gyroscope data
 *   - Attitude estimation (complementary filter)
 *   - Tap/double-tap detection
 *   - Free-fall detection
 */

#ifndef IMU_H
#define IMU_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** IMU sensor reading. */
typedef struct {
    float accel_x, accel_y, accel_z;   /* m/s^2 */
    float gyro_x, gyro_y, gyro_z;      /* rad/s */
    float pitch, roll, yaw;            /* degrees */
    uint32_t timestamp_ms;
} imu_data_t;

/** Initialize BMI270 over I2C. */
void imu_init(void);

/** Get latest IMU reading. Returns false if no data yet. */
bool imu_get_latest(imu_data_t *out);

/** IMU sensor task (Core 1, 200Hz). */
void task_imu(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* IMU_H */
