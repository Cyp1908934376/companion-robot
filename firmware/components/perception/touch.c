/**
 * Capacitive touch sensor — ESP32-S3 built-in touch pads.
 *
 * Four touch zones: head, back, left, right.
 * 50Hz polling with debounce (2 consecutive readings).
 * Long-press detection (>500ms).
 */

#include "touch.h"
#include "config.h"
#include "esp_log.h"
#include "driver/touch_pad.h"
#include "driver/touch_sensor.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_task_wdt.h"

static const char *TAG = "touch";

/* ── Touch pad mapping ────────────────────────────────────────── */

static const touch_pad_t TOUCH_PADS[TOUCH_ZONE_COUNT] = {
    [TOUCH_ZONE_HEAD]  = TOUCH_ZONE_HEAD,   /* TOUCH_PAD_NUM9 */
    [TOUCH_ZONE_BACK]  = TOUCH_ZONE_BACK,   /* TOUCH_PAD_NUM8 */
    [TOUCH_ZONE_LEFT]  = TOUCH_ZONE_LEFT,   /* TOUCH_PAD_NUM7 */
    [TOUCH_ZONE_RIGHT] = TOUCH_ZONE_RIGHT,  /* TOUCH_PAD_NUM6 */
};

/* ── State ────────────────────────────────────────────────────── */

#define TOUCH_THRESHOLD_FACTOR  0.65f   /* touch detected when value < 65% of baseline */
#define DEBOUNCE_COUNT          2
#define LONG_PRESS_MS           500

typedef struct {
    uint16_t baseline;       /* calibrated baseline (untouched value) */
    uint16_t raw_value;      /* latest raw reading */
    bool     touched;        /* debounced touch state */
    uint8_t  debounce_ctr;   /* consecutive readings confirming state */
    uint32_t touch_start_ms; /* tick when touch began */
} touch_zone_state_t;

static touch_zone_state_t g_zones[TOUCH_ZONE_COUNT];

extern QueueHandle_t q_sensor_event;

/* ── Public API ───────────────────────────────────────────────── */

void touch_init(void) {
    ESP_LOGI(TAG, "initializing capacitive touch pads");

    /* Initialize touch peripheral */
    esp_err_t err = touch_pad_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch_pad_init failed: %d", err);
        return;
    }

    /* Configure each touch pad */
    for (int i = 0; i < TOUCH_ZONE_COUNT; i++) {
        touch_pad_config(TOUCH_PADS[i]);
        g_zones[i].baseline     = 0;
        g_zones[i].raw_value    = 0;
        g_zones[i].touched      = false;
        g_zones[i].debounce_ctr = 0;
        g_zones[i].touch_start_ms = 0;
    }

    /* Set filter period to 10ms */
    touch_pad_filter_start(10);

    /* Calibrate baselines */
    vTaskDelay(pdMS_TO_TICKS(100));
    for (int i = 0; i < TOUCH_ZONE_COUNT; i++) {
        uint16_t val;
        if (touch_pad_read_raw_data(TOUCH_PADS[i], &val) == ESP_OK) {
            g_zones[i].baseline = val;
        }
    }

    ESP_LOGI(TAG, "touch pads initialized (head=%d back=%d left=%d right=%d)",
             g_zones[0].baseline, g_zones[1].baseline,
             g_zones[2].baseline, g_zones[3].baseline);
}

void task_touch(void *arg) {
    ESP_LOGI(TAG, "touch task started (50Hz)");

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(20);  /* 50Hz */

    while (1) {
        for (int i = 0; i < TOUCH_ZONE_COUNT; i++) {
            uint16_t raw;
            if (touch_pad_read_raw_data(TOUCH_PADS[i], &raw) != ESP_OK) {
                continue;
            }
            g_zones[i].raw_value = raw;

            /* Touch detection: value drops below threshold when touched */
            uint16_t threshold = (uint16_t)(g_zones[i].baseline * TOUCH_THRESHOLD_FACTOR);
            bool raw_touched = (raw < threshold);

            /* Debounce */
            if (raw_touched != g_zones[i].touched) {
                g_zones[i].debounce_ctr++;
                if (g_zones[i].debounce_ctr >= DEBOUNCE_COUNT) {
                    g_zones[i].touched = raw_touched;
                    g_zones[i].debounce_ctr = 0;

                    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

                    if (raw_touched) {
                        g_zones[i].touch_start_ms = now;
                        ESP_LOGD(TAG, "zone %d TOUCH", i);
                    } else {
                        uint32_t duration = now - g_zones[i].touch_start_ms;
                        ESP_LOGD(TAG, "zone %d RELEASE (held %lu ms)", i, duration);

                        /* Queue touch event */
                        touch_event_t event = {
                            .zone         = (touch_zone_t)i,
                            .touched      = false,
                            .duration_ms  = duration,
                            .timestamp_ms = now,
                        };
                        xQueueSend(q_sensor_event, &event, 0);
                    }
                }
            } else {
                g_zones[i].debounce_ctr = 0;
            }

            /* Long-press detection */
            if (g_zones[i].touched) {
                uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
                uint32_t held = now - g_zones[i].touch_start_ms;
                if (held > LONG_PRESS_MS && (held % 200) < 20) {
                    /* Fire long-press event every 200ms while held */
                    touch_event_t event = {
                        .zone         = (touch_zone_t)i,
                        .touched      = true,
                        .duration_ms  = held,
                        .timestamp_ms = now,
                    };
                    xQueueSend(q_sensor_event, &event, 0);
                }
            }

            /* Slowly adapt baseline (drift compensation) */
            if (!g_zones[i].touched && raw > g_zones[i].baseline) {
                g_zones[i].baseline += 1;  /* slow upward drift */
            }
        }

        vTaskDelayUntil(&last_wake, period);
        esp_task_wdt_reset();
    }
}
