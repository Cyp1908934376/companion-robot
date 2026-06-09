/**
 * Camera driver — OV2640 over SPI.
 *
 * Captures JPEG frames using the ESP32 camera driver.
 * Frame size: QQVGA (160x120) for low latency, QVGA (320x240) for quality.
 * Frame buffer allocated in PSRAM.
 */

#include "camera.h"
#include "config.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_task_wdt.h"
#include <string.h>

static const char *TAG = "camera";

/* ── Camera state ─────────────────────────────────────────────── */

static camera_config_t g_camera_config;
static bool g_camera_ok;
static camera_frame_t g_current_frame;

extern QueueHandle_t q_vision_frame;

/* ── Public API ───────────────────────────────────────────────── */

void camera_init(void) {
    ESP_LOGI(TAG, "initializing OV2640 camera (SPI)");

    g_camera_config = (camera_config_t){
        .pin_pwdn     = -1,
        .pin_reset    = -1,
        .pin_xclk     = -1,
        .pin_sscb_sda = -1,
        .pin_sscb_scl = -1,

        /* SPI pins from config.h */
        .pin_d7   = CAM_MOSI_IO,
        .pin_d6   = CAM_MISO_IO,
        .pin_d5   = -1,
        .pin_d4   = -1,
        .pin_d3   = -1,
        .pin_d2   = -1,
        .pin_d1   = -1,
        .pin_d0   = -1,
        .pin_vsync = -1,
        .pin_href  = -1,
        .pin_pclk  = CAM_SCK_IO,
        .pin_xclk  = -1,

        .xclk_freq_hz    = 20000000,
        .ledc_timer      = LEDC_TIMER_0,
        .ledc_channel    = LEDC_CHANNEL_0,

        .pixel_format    = PIXFORMAT_JPEG,
        .frame_size      = FRAMESIZE_QQVGA,   /* 160x120 */
        .jpeg_quality    = 12,                 /* 0-63, lower = better */
        .fb_count        = 1,                  /* single frame buffer */
        .fb_location     = CAMERA_FB_IN_PSRAM,
        .grab_mode       = CAMERA_GRAB_WHEN_EMPTY,

        .sccb_i2c_port   = I2C_PORT,
    };

    /* Initialize camera */
    esp_err_t err = esp_camera_init(&g_camera_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "camera init failed (err=0x%X) — camera not connected?", err);
        g_camera_ok = false;
        return;
    }

    /* Configure additional sensor settings */
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_brightness(s, 0);       /* -2 to 2 */
        s->set_contrast(s, 0);         /* -2 to 2 */
        s->set_saturation(s, 0);       /* -2 to 2 */
        s->set_special_effect(s, 0);   /* 0 = no effect */
        s->set_whitebal(s, 1);         /* auto white balance */
        s->set_awb_gain(s, 1);         /* auto gain */
        s->set_wb_mode(s, 0);          /* auto */
        s->set_exposure_ctrl(s, 1);    /* auto exposure */
        s->set_aec2(s, 0);
        s->set_gain_ctrl(s, 1);        /* auto gain */
        s->set_agc_gain(s, 0);
        s->set_gainceiling(s, (gainceiling_t)0);
        s->set_bpc(s, 0);
        s->set_wpc(s, 1);
        s->set_raw_gma(s, 1);
        s->set_lenc(s, 1);
        s->set_hmirror(s, 0);
        s->set_vflip(s, 0);
        s->set_dcw(s, 1);
        s->set_colorbar(s, 0);
    }

    g_camera_ok = true;
    g_current_frame = (camera_frame_t){ .data = NULL, .len = 0, .timestamp_ms = 0 };

    ESP_LOGI(TAG, "camera initialized (QQVGA JPEG)");
}

void task_vision(void *arg) {
    ESP_LOGI(TAG, "vision task started (5fps)");

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(200);  /* ~5fps */
    int frame_count = 0;

    while (1) {
        if (g_camera_ok) {
            /* Capture frame */
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb) {
                /* Copy frame data for queue (fb is volatile, returned to driver) */
                camera_frame_t frame = {
                    .data         = fb->buf,
                    .len          = fb->len,
                    .timestamp_ms = xTaskGetTickCount() * portTICK_PERIOD_MS,
                };

                /* Queue frame for behavior engine or uplink */
                if (xQueueSend(q_vision_frame, &frame, 0) != pdTRUE) {
                    /* Queue full — frame dropped. Return buffer and skip processing. */
                    esp_camera_fb_return(fb);
                } else {
                    /* Successful queue — frame data ownership transferred.
                     * In production: frame data should be copied to a ring buffer.
                     * For now, we note that the queue stores a pointer and the fb
                     * will be returned after the consumer processes it. */
                    frame_count++;
                }

                /* Return the frame buffer to the camera driver after consumer is done
                 * NOTE: In production, use a proper frame lifecycle manager.
                 * The behavior engine must call esp_camera_fb_return() after processing. */
            } else {
                ESP_LOGW(TAG, "camera capture failed");
            }
        }

        /* Basic frame statistics every 5s */
        if (frame_count > 0 && (frame_count % 25) == 0) {  /* every 5s at 5fps */
            ESP_LOGD(TAG, "captured %d frames", frame_count);
        }

        vTaskDelayUntil(&last_wake, period);
        esp_task_wdt_reset();
    }
}
