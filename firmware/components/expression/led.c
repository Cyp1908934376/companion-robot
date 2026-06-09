/**
 * LED driver — WS2812B RGB LEDs (8x) via RMT.
 *
 * Patterns: solid, breathing, blinking, rainbow, knight_rider, party.
 * RMT timing: T0H=350ns T0L=900ns, T1H=900ns T1L=350ns, RESET>50us.
 * GRB color order per WS2812B datasheet.
 */

#include "led.h"
#include "config.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_task_wdt.h"
#include <string.h>
#include <math.h>

static const char *TAG = "led";

/* ── RMT constants ────────────────────────────────────────────── */

#define RMT_RESOLUTION_HZ   80000000  /* 80MHz APB clock */
#define RMT_T0H_NS          350
#define RMT_T0L_NS          900
#define RMT_T1H_NS          900
#define RMT_T1L_NS          350
#define WS2812_RESET_US     60

/* RMT symbol durations in ticks */
#define T0H_TICKS  ((RMT_RESOLUTION_HZ / 1000000000ULL) * RMT_T0H_NS / 1000)
#define T0L_TICKS  ((RMT_RESOLUTION_HZ / 1000000000ULL) * RMT_T0L_NS / 1000)
#define T1H_TICKS  ((RMT_RESOLUTION_HZ / 1000000000ULL) * RMT_T1H_NS / 1000)
#define T1L_TICKS  ((RMT_RESOLUTION_HZ / 1000000000ULL) * RMT_T1L_NS / 1000)

/* Encoded buffer: 24 bits per LED × 8 LEDs × 2 RMT symbols per bit */
#define BITS_PER_LED    24
#define LED_BUF_BYTES    (LED_COUNT * 3)           /* 8 × RGB = 24 bytes */
#define RMT_SYMBOLS      (LED_COUNT * BITS_PER_LED * 2)  /* 8 × 24 × 2 = 384 */

/* ── LED state ────────────────────────────────────────────────── */

static rmt_channel_handle_t g_rmt_chan;
static rmt_encoder_handle_t g_encoder;
static bool g_rmt_ok;

static uint8_t g_led_buf[LED_BUF_BYTES];  /* GRB order, 3 bytes per LED */

/* Pattern state */
static led_pattern_t g_pattern;
static bool           g_pattern_active;
static uint32_t       g_pattern_start_ms;

/* ── RMT encoder (bytes → WS2812 symbols) ─────────────────────── */

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_symbol;
} ws2812_encoder_t;

static size_t ws2812_encode(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                            const void *data, size_t data_size, rmt_encode_state_t *ret_state) {
    ws2812_encoder_t *ws = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    size_t encoded = 0;

    /* Encode RGB bytes into RMT symbols */
    encoded += ws->bytes_encoder->encode(ws->bytes_encoder, channel, data, data_size, &session_state);

    if (session_state & RMT_ENCODING_COMPLETE) {
        /* Append reset code (low for >50us) */
        encoded += ws->copy_encoder->encode(ws->copy_encoder, channel,
                                            &ws->reset_symbol, sizeof(ws->reset_symbol),
                                            &session_state);
        if (session_state & RMT_ENCODING_MEM_FULL) {
            *ret_state = RMT_ENCODING_MEM_FULL;
            return encoded;
        }
        *ret_state = RMT_ENCODING_COMPLETE;
    }
    return encoded;
}

static esp_err_t ws2812_encoder_del(rmt_encoder_t *encoder) {
    ws2812_encoder_t *ws = __containerof(encoder, ws2812_encoder_t, base);
    ws->bytes_encoder->del(ws->bytes_encoder);
    ws->copy_encoder->del(ws->copy_encoder);
    free(ws);
    return ESP_OK;
}

static esp_err_t ws2812_encoder_reset(rmt_encoder_t *encoder) {
    ws2812_encoder_t *ws = __containerof(encoder, ws2812_encoder_t, base);
    ws->bytes_encoder->reset(ws->bytes_encoder);
    ws->copy_encoder->reset(ws->copy_encoder);
    ws->state = 0;
    return ESP_OK;
}

/* ── RGB → WS2812 bitstream ───────────────────────────────────── */

/**
 * Convert a single RGB byte (8 bits) to a sequence of RMT symbols.
 * Each bit → {T0H, T0L} or {T1H, T1L}.
 */
static size_t encode_byte(const uint8_t *src, rmt_symbol_word_t *dest, size_t dest_size,
                          size_t *src_offset) {
    size_t bit;
    size_t encoded = 0;
    uint8_t byte = src[*src_offset];
    (*src_offset)++;

    for (bit = 0; bit < 8; bit++) {
        if (encoded * sizeof(rmt_symbol_word_t) >= dest_size) {
            return 0;  /* buffer full */
        }
        if (byte & (1 << (7 - bit))) {
            /* Logical 1 */
            dest[encoded].duration0 = T1H_TICKS;
            dest[encoded].duration1 = T1L_TICKS;
        } else {
            /* Logical 0 */
            dest[encoded].duration0 = T0H_TICKS;
            dest[encoded].duration1 = T0L_TICKS;
        }
        encoded++;
    }
    return encoded;
}

/* ── Color helpers ────────────────────────────────────────────── */

static void rgb_to_grb(uint8_t r, uint8_t g, uint8_t b, int led_idx) {
    /* WS2812B expects GRB order */
    g_led_buf[led_idx * 3 + 0] = g;
    g_led_buf[led_idx * 3 + 1] = r;
    g_led_buf[led_idx * 3 + 2] = b;
}

static void set_all_grb(const uint8_t *grb) {
    for (int i = 0; i < LED_COUNT; i++) {
        memcpy(&g_led_buf[i * 3], grb, 3);
    }
}

static void transmit_leds(void) {
    if (!g_rmt_ok) return;

    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
        .flags.eot_level = 0,
    };

    rmt_transmit(g_rmt_chan, g_encoder, g_led_buf, LED_BUF_BYTES, &tx_cfg);
    /* Wait for transmit to complete (each LED = 24 bits × ~1.25us = 30us, 8 LEDs = 240us) */
    rmt_tx_wait_all_done(g_rmt_chan, pdMS_TO_TICKS(10));
}

/* ── Pattern helpers ──────────────────────────────────────────── */

static void hue_to_rgb(float h, uint8_t *r, uint8_t *g, uint8_t *b) {
    /* h: 0-360, s=1.0, v=1.0 */
    float c = 1.0f;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = 0.0f;

    float r1, g1, b1;
    if (h < 60)       { r1 = c; g1 = x; b1 = 0; }
    else if (h < 120) { r1 = x; g1 = c; b1 = 0; }
    else if (h < 180) { r1 = 0; g1 = c; b1 = x; }
    else if (h < 240) { r1 = 0; g1 = x; b1 = c; }
    else if (h < 300) { r1 = x; g1 = 0; b1 = c; }
    else              { r1 = c; g1 = 0; b1 = x; }

    *r = (uint8_t)((r1 + m) * 255.0f);
    *g = (uint8_t)((g1 + m) * 255.0f);
    *b = (uint8_t)((b1 + m) * 255.0f);
}

static void update_pattern(void) {
    if (!g_pattern_active) return;

    uint32_t elapsed = (xTaskGetTickCount() * portTICK_PERIOD_MS) - g_pattern_start_ms;
    float speed = g_pattern.speed / 255.0f;
    uint8_t r = g_pattern.color.r;
    uint8_t g = g_pattern.color.g;
    uint8_t b = g_pattern.color.b;

    switch (g_pattern.mode) {
    case BCP_LED_SOLID:
        for (int i = 0; i < LED_COUNT; i++) {
            rgb_to_grb(r, g, b, i);
        }
        break;

    case BCP_LED_BREATHING: {
        float phase = (elapsed / 1000.0f) * (speed + 0.5f) * 2.0f * 3.14159265f;
        float brightness = (sinf(phase) + 1.0f) / 2.0f;  /* 0..1 */
        uint8_t br = (uint8_t)(brightness * 255.0f);
        for (int i = 0; i < LED_COUNT; i++) {
            rgb_to_grb((uint8_t)(r * br / 255), (uint8_t)(g * br / 255), (uint8_t)(b * br / 255), i);
        }
        break;
    }

    case BCP_LED_BLINK: {
        float period_ms = 1000.0f / (speed + 0.5f);
        bool on = fmodf(elapsed, period_ms * 2.0f) < period_ms;
        uint8_t val = on ? 255 : 0;
        for (int i = 0; i < LED_COUNT; i++) {
            rgb_to_grb(r & val, g & val, b & val, i);
        }
        break;
    }

    case BCP_LED_RAINBOW: {
        float hue_per_led = 360.0f / LED_COUNT;
        float hue_offset = elapsed * speed * 0.05f;  /* rotating rainbow */
        for (int i = 0; i < LED_COUNT; i++) {
            float h = fmodf(hue_offset + i * hue_per_led, 360.0f);
            uint8_t lr, lg, lb;
            hue_to_rgb(h, &lr, &lg, &lb);
            rgb_to_grb(lr, lg, lb, i);
        }
        break;
    }

    case BCP_LED_KNIGHT_RIDER: {
        int pos = (int)(elapsed * speed / 50.0f) % (LED_COUNT * 2 - 2);
        if (pos >= LED_COUNT) pos = LED_COUNT * 2 - 2 - pos;
        for (int i = 0; i < LED_COUNT; i++) {
            if (i == pos) {
                rgb_to_grb(r, g, b, i);
            } else if (i == pos - 1 || i == pos + 1) {
                rgb_to_grb(r / 3, g / 3, b / 3, i);
            } else {
                rgb_to_grb(0, 0, 0, i);
            }
        }
        break;
    }

    case BCP_LED_PARTY: {
        static uint32_t last_switch;
        if (elapsed - last_switch > 200 / (speed + 1)) {
            last_switch = elapsed;
            for (int i = 0; i < LED_COUNT; i++) {
                uint8_t cr, cg, cb;
                hue_to_rgb((float)(esp_random() % 360), &cr, &cg, &cb);
                rgb_to_grb(cr, cg, cb, i);
            }
        }
        break;
    }

    default:
        break;
    }
}

/* ── Public API ───────────────────────────────────────────────── */

void led_init(void) {
    ESP_LOGI(TAG, "initializing WS2812B LEDs (RMT)");

    /* Configure RMT TX channel */
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num          = LED_DATA_IO,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 512,  /* enough for 8 LEDs × 24 × 2 = 384 symbols */
        .trans_queue_depth = 2,
    };

    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &g_rmt_chan);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RMT TX channel failed: %d", err);
        g_rmt_ok = false;
        return;
    }

    /* Create byte encoder (generic bytes → RMT copy) */
    rmt_bytes_encoder_config_t bytes_cfg = {
        .bit0 = { .duration0 = T0H_TICKS, .duration1 = T0L_TICKS },
        .bit1 = { .duration0 = T1H_TICKS, .duration1 = T1L_TICKS },
        .flags.msb_first = true,
    };

    rmt_encoder_handle_t bytes_encoder;
    rmt_new_bytes_encoder(&bytes_cfg, &bytes_encoder);

    /* Create copy encoder for reset signal */
    rmt_copy_encoder_config_t copy_cfg = {};
    rmt_encoder_handle_t copy_encoder;
    rmt_new_copy_encoder(&copy_cfg, &copy_encoder);

    /* Create WS2812 encoder */
    ws2812_encoder_t *ws_encoder = calloc(1, sizeof(ws2812_encoder_t));
    ws_encoder->base.encode       = ws2812_encode;
    ws_encoder->base.del          = ws2812_encoder_del;
    ws_encoder->base.reset        = ws2812_encoder_reset;
    ws_encoder->bytes_encoder     = bytes_encoder;
    ws_encoder->copy_encoder      = copy_encoder;
    ws_encoder->reset_symbol      = (rmt_symbol_word_t){
        .duration0 = WS2812_RESET_US * (RMT_RESOLUTION_HZ / 1000000) / 1000,
        .duration1 = 0,
    };
    g_encoder = &ws_encoder->base;

    /* Enable RMT TX */
    err = rmt_enable(g_rmt_chan);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RMT enable failed: %d", err);
        g_rmt_ok = false;
        return;
    }

    /* Clear LEDs */
    memset(g_led_buf, 0, LED_BUF_BYTES);
    g_pattern_active = false;

    g_rmt_ok = true;
    ESP_LOGI(TAG, "WS2812B LEDs ready (RMT, %d LEDs)", LED_COUNT);
}

void led_set_solid(led_color_t color) {
    for (int i = 0; i < LED_COUNT; i++) {
        rgb_to_grb(color.r, color.g, color.b, i);
    }
    g_pattern_active = false;
    transmit_leds();
}

void led_set_pattern(led_pattern_t pattern) {
    g_pattern = pattern;
    g_pattern_active = true;
    g_pattern_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    ESP_LOGD(TAG, "pattern: mode=%d speed=%d color=RGB(%d,%d,%d)",
             pattern.mode, pattern.speed, pattern.color.r, pattern.color.g, pattern.color.b);
}

void led_off(void) {
    memset(g_led_buf, 0, LED_BUF_BYTES);
    g_pattern_active = false;
    transmit_leds();
}

/* ── LED task ─────────────────────────────────────────────────── */

void task_led(void *arg) {
    ESP_LOGI(TAG, "LED task started (20Hz)");

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(50);  /* 20Hz */

    while (1) {
        if (g_pattern_active) {
            update_pattern();
            transmit_leds();
        }

        vTaskDelayUntil(&last_wake, period);
        esp_task_wdt_reset();
    }
}
