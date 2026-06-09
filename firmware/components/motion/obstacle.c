/**
 * Obstacle detection — fusion of ToF + IMU + camera depth.
 *
 * Reads 3x VL53L0X ToF sensors and maps to 5 directional zones.
 * Hysteresis: 2 consecutive readings required for state change.
 * Thresholds: STOP=10cm, SLOW=30cm, SAFE=50cm.
 */

#include "obstacle.h"
#include "tof.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/task.h"

static const char *TAG = "obstacle";

/* ── Hysteresis state ─────────────────────────────────────────── */

#define HYSTERESIS_COUNT 2

typedef struct {
    bool     pending_stop[OBS_DIR_COUNT];   /* candidate emergency_stop */
    bool     pending_slow[OBS_DIR_COUNT];   /* candidate slow_down */
    uint8_t  stop_counter[OBS_DIR_COUNT];
    uint8_t  slow_counter[OBS_DIR_COUNT];
    uint16_t prev_distance[OBS_DIR_COUNT];  /* for logging deltas */
} hysteresis_t;

static hysteresis_t g_hyst;

/* ── Direction mapping ────────────────────────────────────────── */

/**
 * Map 3 physical ToF sensors to 5 logical obstacle directions.
 *
 * Physical layout (top-down view):
 *       TOF1 (front-center)
 *   TOF2 (front-left)  TOF3 (front-right)
 *
 * Logical zones:
 *   OBS_FRONT       ← avg(TOF1, min(TOF2, TOF3))
 *   OBS_FRONT_LEFT  ← TOF2
 *   OBS_FRONT_RIGHT ← TOF3
 *   OBS_REAR        ← not covered by ToF (use camera depth or set safe)
 */
static void map_tof_to_directions(const tof_reading_t *tof,
                                  uint16_t distances[OBS_DIR_COUNT]) {
    /* Front: closest of all 3 sensors (conservative) */
    uint16_t front_min = tof->distance_mm[0];
    if (tof->distance_mm[1] < front_min) front_min = tof->distance_mm[1];
    if (tof->distance_mm[2] < front_min) front_min = tof->distance_mm[2];

    distances[OBS_FRONT]       = front_min;
    distances[OBS_FRONT_LEFT]  = tof->distance_mm[1];  /* sensor 2 = front-left */
    distances[OBS_FRONT_RIGHT] = tof->distance_mm[2];  /* sensor 3 = front-right */
    distances[OBS_REAR]        = OBSTACLE_SAFE_CM + 1; /* no rear sensor yet */
}

/* ── Public API ───────────────────────────────────────────────── */

void obstacle_init(void) {
    ESP_LOGI(TAG, "initializing obstacle detection");
    g_hyst = (hysteresis_t){0};
}

obstacle_status_t obstacle_check(void) {
    obstacle_status_t status = {0};
    tof_reading_t tof_data;

    if (!tof_read_all(&tof_data)) {
        ESP_LOGW(TAG, "ToF read failed — using safe defaults");
        for (int i = 0; i < OBS_DIR_COUNT; i++) {
            status.distance_cm[i] = OBSTACLE_SAFE_CM + 1;
        }
        status.timestamp_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        return status;
    }

    /* Map physical ToF to logical directions (convert mm → cm) */
    uint16_t raw_mm[OBS_DIR_COUNT];
    map_tof_to_directions(&tof_data, raw_mm);

    for (int i = 0; i < OBS_DIR_COUNT; i++) {
        status.distance_cm[i] = raw_mm[i] / 10;
    }

    /* ── Hysteresis: emergency stop ────────────────────────────── */
    for (int i = 0; i < OBS_DIR_COUNT; i++) {
        if (status.distance_cm[i] > 0 && status.distance_cm[i] <= OBSTACLE_STOP_CM) {
            g_hyst.stop_counter[i]++;
            if (g_hyst.stop_counter[i] >= HYSTERESIS_COUNT) {
                g_hyst.pending_stop[i] = true;
            }
        } else {
            g_hyst.stop_counter[i] = 0;
            g_hyst.pending_stop[i] = false;
        }
    }

    /* ── Hysteresis: slow down ─────────────────────────────────── */
    for (int i = 0; i < OBS_DIR_COUNT; i++) {
        if (status.distance_cm[i] > 0
            && status.distance_cm[i] > OBSTACLE_STOP_CM
            && status.distance_cm[i] <= OBSTACLE_SLOW_CM) {
            g_hyst.slow_counter[i]++;
            if (g_hyst.slow_counter[i] >= HYSTERESIS_COUNT) {
                g_hyst.pending_slow[i] = true;
            }
        } else {
            g_hyst.slow_counter[i] = 0;
            g_hyst.pending_slow[i] = false;
        }
    }

    /* ── Set flags ─────────────────────────────────────────────── */
    status.emergency_stop = false;
    status.slow_down      = false;
    for (int i = 0; i < OBS_DIR_COUNT; i++) {
        if (g_hyst.pending_stop[i]) status.emergency_stop = true;
        if (g_hyst.pending_slow[i]) status.slow_down      = true;
    }

    status.timestamp_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    /* Log when obstacle enters/leaves stop zone */
    for (int i = 0; i < OBS_DIR_COUNT; i++) {
        int16_t delta = (int16_t)status.distance_cm[i] - (int16_t)g_hyst.prev_distance[i];
        if (abs(delta) > 10) {
            ESP_LOGD(TAG, "dir=%d dist=%d cm (delta=%d)",
                     i, status.distance_cm[i], delta);
        }
        g_hyst.prev_distance[i] = status.distance_cm[i];
    }

    if (status.emergency_stop) {
        ESP_LOGW(TAG, "EMERGENCY STOP — obstacle within %d cm", OBSTACLE_STOP_CM);
    } else if (status.slow_down) {
        ESP_LOGD(TAG, "SLOW ZONE — obstacle within %d cm", OBSTACLE_SLOW_CM);
    }

    return status;
}
