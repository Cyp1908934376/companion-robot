/**
 * Microphone driver — INMP441 4ch array over I2S.
 *
 * 16kHz 16-bit mono capture with double-buffered DMA.
 * Voice Activity Detection (VAD) via RMS energy threshold.
 * Audio chunks queued for uplink via BCP.
 */

#include "microphone.h"
#include "config.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_task_wdt.h"
#include <string.h>
#include <math.h>

static const char *TAG = "mic";

/* ── Audio constants ──────────────────────────────────────────── */

#define AUDIO_SAMPLE_RATE      16000
#define AUDIO_CHUNK_MS         100       /* 100ms chunks */
#define AUDIO_SAMPLES_PER_CHUNK (AUDIO_SAMPLE_RATE * AUDIO_CHUNK_MS / 1000)  /* 1600 */
#define DMA_BUF_COUNT          2
#define DMA_BUF_SIZE           (AUDIO_SAMPLES_PER_CHUNK * sizeof(int16_t))  /* 3200 bytes */

/* ── VAD parameters ───────────────────────────────────────────── */

#define VAD_FRAME_MS           20        /* 20ms analysis frames */
#define VAD_SAMPLES_PER_FRAME  (AUDIO_SAMPLE_RATE * VAD_FRAME_MS / 1000)  /* 320 */
#define VAD_THRESHOLD_FACTOR   3.0f      /* speech RMS > 3x noise floor */

/* ── Audio state ──────────────────────────────────────────────── */

static i2s_chan_handle_t g_rx_chan;
static bool g_i2s_ok;

/* VAD state */
static float g_noise_floor;             /* running estimate of background noise */
static bool  g_speech_active;           /* currently in speech segment */
static uint32_t g_speech_start_ms;      /* when current speech segment started */

/* DMA buffers in PSRAM */
static int16_t *g_dma_buf[2];
static int g_active_buf;

extern QueueHandle_t q_audio_frame;

/* ── Public API ───────────────────────────────────────────────── */

void microphone_init(void) {
    ESP_LOGI(TAG, "initializing INMP441 microphone array (I2S)");

    /* Configure I2S RX channel: standard Philips, 16kHz, 16-bit, mono */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &g_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %d", err);
        g_i2s_ok = false;
        return;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_MIC_BCK_IO,
            .ws   = I2S_MIC_WS_IO,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_MIC_DATA_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(g_rx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %d", err);
        g_i2s_ok = false;
        return;
    }

    /* Allocate DMA buffers in PSRAM */
    for (int i = 0; i < DMA_BUF_COUNT; i++) {
        g_dma_buf[i] = heap_caps_malloc(DMA_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!g_dma_buf[i]) {
            ESP_LOGW(TAG, "PSRAM allocation failed — using internal DRAM");
            g_dma_buf[i] = malloc(DMA_BUF_SIZE);
        }
        if (!g_dma_buf[i]) {
            ESP_LOGE(TAG, "DMA buffer allocation failed");
            g_i2s_ok = false;
            return;
        }
        memset(g_dma_buf[i], 0, DMA_BUF_SIZE);
    }

    g_active_buf = 0;
    g_noise_floor = 100.0f;   /* initial noise estimate */
    g_speech_active = false;

    /* Enable I2S RX */
    err = i2s_channel_enable(g_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %d", err);
        g_i2s_ok = false;
        return;
    }

    g_i2s_ok = true;
    ESP_LOGI(TAG, "microphone ready (16kHz 16-bit mono, %dms chunks)", AUDIO_CHUNK_MS);
}

/* ── Voice Activity Detection ─────────────────────────────────── */

/**
 * Compute RMS energy of an audio buffer.
 */
static float compute_rms(const int16_t *samples, size_t count) {
    float sum = 0.0f;
    for (size_t i = 0; i < count; i++) {
        sum += (float)samples[i] * (float)samples[i];
    }
    return sqrtf(sum / (float)count);
}

/**
 * Simple energy-based VAD.
 * Returns true if speech is detected in the frame.
 */
static bool vad_detect(const int16_t *samples, size_t count) {
    float rms = compute_rms(samples, count);

    /* Update noise floor (slow tracking of minimum energy) */
    if (!g_speech_active && rms < g_noise_floor * 1.2f) {
        g_noise_floor = g_noise_floor * 0.95f + rms * 0.05f;  /* exponential moving average */
    }

    return rms > g_noise_floor * VAD_THRESHOLD_FACTOR;
}

/* ── Audio input task ─────────────────────────────────────────── */

void task_audio_in(void *arg) {
    ESP_LOGI(TAG, "audio_in task started");

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(AUDIO_CHUNK_MS);  /* 100ms */

    while (1) {
        if (!g_i2s_ok) {
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_task_wdt_reset();
            continue;
        }

        /* Read audio chunk from I2S */
        size_t bytes_read = 0;
        int16_t *buf = g_dma_buf[g_active_buf];
        esp_err_t err = i2s_channel_read(g_rx_chan, buf,
                                         DMA_BUF_SIZE, &bytes_read,
                                         pdMS_TO_TICKS(AUDIO_CHUNK_MS + 10));

        if (err != ESP_OK || bytes_read == 0) {
            /* I2S underrun or timeout — retry next cycle */
            static int i2s_err_count = 0;
            if (++i2s_err_count % 10 == 0) {
                ESP_LOGW(TAG, "I2S read error: %d", err);
            }
            vTaskDelayUntil(&last_wake, period);
            esp_task_wdt_reset();
            continue;
        }

        size_t sample_count = bytes_read / sizeof(int16_t);
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

        /* Run VAD on this chunk */
        bool has_speech = false;
        for (size_t offset = 0; offset + VAD_SAMPLES_PER_FRAME <= sample_count;
             offset += VAD_SAMPLES_PER_FRAME) {
            if (vad_detect(buf + offset, VAD_SAMPLES_PER_FRAME)) {
                has_speech = true;
                break;
            }
        }

        /* Speech state machine */
        if (has_speech && !g_speech_active) {
            /* Speech start */
            g_speech_active = true;
            g_speech_start_ms = now_ms;
            ESP_LOGD(TAG, "speech start");
        } else if (!has_speech && g_speech_active) {
            /* Speech end */
            g_speech_active = false;
            uint32_t duration = now_ms - g_speech_start_ms;
            ESP_LOGD(TAG, "speech end (duration=%lu ms)", duration);
        }

        /* Queue audio chunk for uplink if speech active or periodic */
        if (g_speech_active || (xTaskGetTickCount() % 50 == 0)) {
            audio_chunk_t chunk = {
                .samples      = buf,
                .sample_count = sample_count,
                .channels     = 1,
                .timestamp_ms = now_ms,
            };

            if (xQueueSend(q_audio_frame, &chunk, 0) != pdTRUE) {
                /* Queue full — drop this chunk */
            } else {
                /* Switch to next buffer */
                g_active_buf = (g_active_buf + 1) % DMA_BUF_COUNT;
            }
        }

        vTaskDelayUntil(&last_wake, period);
        esp_task_wdt_reset();
    }
}
