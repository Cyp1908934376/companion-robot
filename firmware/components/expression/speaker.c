/**
 * Speaker driver — MAX98357A I2S Class-D amplifier.
 *
 * 22.05kHz 16-bit mono output.
 * Supports WAV file playback from SPIFFS and TTS streaming.
 * Double-buffered DMA, silence padding on underrun.
 */

#include "speaker.h"
#include "config.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "esp_spiffs.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_task_wdt.h"
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "speaker";

/* ── Audio constants ──────────────────────────────────────────── */

#define SAMPLE_RATE          22050
#define CHUNK_MS             50
#define SAMPLES_PER_CHUNK    (SAMPLE_RATE * CHUNK_MS / 1000)  /* ~1102 */
#define DMA_BUF_COUNT        2
#define DMA_BUF_SIZE         (SAMPLES_PER_CHUNK * sizeof(int16_t))

/* ── WAV header ───────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    char     riff[4];        /* "RIFF" */
    uint32_t file_size;
    char     wave[4];        /* "WAVE" */
    char     fmt[4];         /* "fmt " */
    uint32_t fmt_size;
    uint16_t audio_format;   /* 1 = PCM */
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char     data[4];        /* "data" */
    uint32_t data_size;
} wav_header_t;

/* ── State ────────────────────────────────────────────────────── */

static i2s_chan_handle_t g_tx_chan;
static bool g_i2s_ok;

/* TTS queue */
#define TTS_QUEUE_DEPTH  8
static QueueHandle_t g_tts_queue;

/* ── Public API ───────────────────────────────────────────────── */

void speaker_init(void) {
    ESP_LOGI(TAG, "initializing MAX98357A speaker (I2S)");

    /* Configure I2S TX channel */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &g_tx_chan, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %d", err);
        g_i2s_ok = false;
        return;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SPK_BCK_IO,
            .ws   = I2S_SPK_WS_IO,
            .dout = I2S_SPK_DATA_IO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(g_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %d", err);
        g_i2s_ok = false;
        return;
    }

    /* Create TTS queue */
    g_tts_queue = xQueueCreate(TTS_QUEUE_DEPTH, sizeof(audio_out_buf_t));

    /* Enable I2S TX */
    err = i2s_channel_enable(g_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %d", err);
        g_i2s_ok = false;
        return;
    }

    g_i2s_ok = true;
    ESP_LOGI(TAG, "speaker ready (22.05kHz 16-bit mono)");
}

void speaker_play_file(const char *path) {
    if (!g_i2s_ok) return;

    ESP_LOGI(TAG, "play file: %s", path);

    /* Open WAV file from SPIFFS */
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "file not found: %s", path);
        return;
    }

    /* Parse WAV header */
    wav_header_t header;
    if (fread(&header, sizeof(header), 1, f) != 1) {
        ESP_LOGW(TAG, "failed to read WAV header");
        fclose(f);
        return;
    }

    if (header.audio_format != 1 || header.bits_per_sample != 16) {
        ESP_LOGW(TAG, "unsupported WAV format (fmt=%d bits=%d)",
                 header.audio_format, header.bits_per_sample);
        fclose(f);
        return;
    }

    ESP_LOGD(TAG, "WAV: %dHz %dch %d-bit, %lu samples",
             header.sample_rate, header.num_channels,
             header.bits_per_sample, header.data_size / header.block_align);

    /* Stream PCM data to I2S in chunks */
    int16_t *buf = malloc(DMA_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "buffer allocation failed");
        fclose(f);
        return;
    }

    size_t total_written = 0;
    while (total_written < header.data_size) {
        size_t remaining = header.data_size - total_written;
        size_t to_read = (remaining < DMA_BUF_SIZE) ? remaining : DMA_BUF_SIZE;

        size_t read = fread(buf, 1, to_read, f);
        if (read == 0) break;

        /* Mono: write directly. Stereo → downmix needed, but we assume mono. */
        size_t bytes_written = 0;
        i2s_channel_write(g_tx_chan, buf, read, &bytes_written, pdMS_TO_TICKS(100));
        total_written += read;
    }

    free(buf);
    fclose(f);

    /* Flush I2S */
    i2s_channel_write(g_tx_chan, NULL, 0, NULL, pdMS_TO_TICKS(100));

    ESP_LOGD(TAG, "playback complete: %s", path);
}

void speaker_enqueue_tts(const int16_t *samples, size_t count) {
    if (!g_i2s_ok) return;

    /* Copy samples to a new buffer for queuing */
    audio_out_buf_t buf = {
        .samples      = malloc(count * sizeof(int16_t)),
        .sample_count = count,
        .tts          = true,
    };

    if (!buf.samples) return;
    memcpy(buf.samples, samples, count * sizeof(int16_t));

    if (xQueueSend(g_tts_queue, &buf, 0) != pdTRUE) {
        free(buf.samples);  /* queue full — drop */
    }
}

/* ── Audio output task ────────────────────────────────────────── */

void task_audio_out(void *arg) {
    ESP_LOGI(TAG, "audio_out task started (20Hz)");

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(CHUNK_MS);

    while (1) {
        if (!g_i2s_ok) {
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_task_wdt_reset();
            continue;
        }

        /* Check for TTS audio in queue */
        audio_out_buf_t buf;
        if (xQueueReceive(g_tts_queue, &buf, 0) == pdTRUE) {
            /* Write TTS samples to I2S */
            size_t bytes_written = 0;
            size_t bytes_total = buf.sample_count * sizeof(int16_t);
            i2s_channel_write(g_tx_chan, buf.samples, bytes_total,
                              &bytes_written, pdMS_TO_TICKS(CHUNK_MS * 2));

            /* Free the TTS buffer */
            free(buf.samples);

            ESP_LOGD(TAG, "TTS playback: %d samples", buf.sample_count);
        }

        vTaskDelayUntil(&last_wake, period);
        esp_task_wdt_reset();
    }
}
