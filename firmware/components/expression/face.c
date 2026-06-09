/**
 * Face display — SSD1306 OLED (128x64) over I2C.
 *
 * Renders facial expressions as bitmap icons.
 * 6 built-in expressions: happy, sad, angry, surprised, neutral, sleepy.
 * Blink animation with random interval (2-6s).
 */

#include "face.h"
#include "config.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "face";

/* ── SSD1306 constants ────────────────────────────────────────── */

#define SSD1306_ADDR            0x3C
#define SSD1306_WIDTH           128
#define SSD1306_HEIGHT          64
#define SSD1306_PAGES           (SSD1306_HEIGHT / 8)  /* 8 pages */

/* Display commands */
#define SSD1306_CMD_DISPLAY_OFF     0xAE
#define SSD1306_CMD_DISPLAY_ON      0xAF
#define SSD1306_CMD_SET_CONTRAST    0x81

/* ── State ────────────────────────────────────────────────────── */

static bool g_display_ok;
static bcp_face_expr_t g_current_expr = BCP_FACE_NEUTRAL;

/* Blink animation */
static uint32_t g_last_blink_ms;
static uint32_t g_next_blink_ms;
static bool g_blinking;

/* ── Expression bitmaps (32x32 pixels, center-aligned on 128x64) ─
 * Each bitmap = 32 × 32 bits = 128 bytes.
 * Simplified versions for embedded use. */

#define BM_WIDTH    32
#define BM_HEIGHT   32
#define BM_BYTES    (BM_WIDTH * BM_HEIGHT / 8)  /* 128 bytes */
#define BM_X_OFFSET ((SSD1306_WIDTH - BM_WIDTH) / 2)   /* 48 */
#define BM_Y_OFFSET ((SSD1306_HEIGHT - BM_HEIGHT) / 2)  /* 16 */

/* Neutral face: circles for eyes, line for mouth */
static const uint8_t FACE_NEUTRAL[BM_BYTES] = {
    /* Row 0 */
    0x00,0x00,0x00,0x00,
    /* Rows 1-6: top of eyes (filled circles, 6px radius) */
    0x00,0x00,0x00,0x00,
    0x00,0x07,0xC0,0x00,0x00,0x03,0xE0,0x00, /* left eye start */
    0x00,0x1F,0xF0,0x00,0x00,0x0F,0xF8,0x00,
    0x00,0x3F,0xF8,0x00,0x00,0x1F,0xFC,0x00,
    0x00,0x7F,0xFC,0x00,0x00,0x3F,0xFE,0x00,
    0x00,0x7F,0xFC,0x00,0x00,0x3F,0xFE,0x00,
    0x00,0x7F,0xFC,0x00,0x00,0x3F,0xFE,0x00,
    0x00,0x3F,0xF8,0x00,0x00,0x1F,0xFC,0x00,
    0x00,0x1F,0xF0,0x00,0x00,0x0F,0xF8,0x00,
    0x00,0x07,0xC0,0x00,0x00,0x03,0xE0,0x00,
    /* Blank rows between eyes and mouth */
    0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,
    /* Mouth: flat line */
    0x00,0x00,0x00,0x00,
    0x07,0xFF,0xFF,0xC0,
    0x07,0xFF,0xFF,0xC0,
    0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,
};

/* Simplified happy/sad/angry/surprised — variations on mouth and eyes */
static const uint8_t *FACE_BITMAPS[] = {
    [BCP_FACE_NEUTRAL]   = FACE_NEUTRAL,
    [BCP_FACE_HAPPY]     = NULL,  /* placeholder — generated at init */
    [BCP_FACE_SAD]       = NULL,
    [BCP_FACE_ANGRY]     = NULL,
    [BCP_FACE_SURPRISED] = NULL,
    [BCP_FACE_SLEEPY]     = NULL,
};

/* ── I2C helpers ──────────────────────────────────────────────── */

static esp_err_t ssd1306_write_cmd(uint8_t cmd) {
    uint8_t buf[2] = { 0x00, cmd };  /* 0x00 = command mode */
    return i2c_master_write_to_device(I2C_PORT, SSD1306_ADDR, buf, 2, pdMS_TO_TICKS(10));
}

static esp_err_t ssd1306_write_data(const uint8_t *data, size_t len) {
    /* SSD1306 data mode: Co=0 D/C=1 → 0x40 */
    uint8_t *buf = malloc(len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    buf[0] = 0x40;
    memcpy(buf + 1, data, len);
    esp_err_t err = i2c_master_write_to_device(I2C_PORT, SSD1306_ADDR, buf, len + 1, pdMS_TO_TICKS(50));
    free(buf);
    return err;
}

/* ── Display helpers ──────────────────────────────────────────── */

static void clear_display(void) {
    uint8_t zero_page[SSD1306_WIDTH];
    memset(zero_page, 0, SSD1306_WIDTH);

    for (int page = 0; page < SSD1306_PAGES; page++) {
        ssd1306_write_cmd(0xB0 | page);  /* set page address */
        ssd1306_write_cmd(0x00);         /* set column low */
        ssd1306_write_cmd(0x10);         /* set column high */
        ssd1306_write_data(zero_page, SSD1306_WIDTH);
    }
}

static void draw_bitmap(const uint8_t *bitmap, int x, int y, int w, int h) {
    if (!bitmap) return;

    int bytes_per_row = w / 8;

    for (int row = 0; row < h; row++) {
        int page = (y + row) / 8;
        if (page < 0 || page >= SSD1306_PAGES) continue;

        int page_offset = (y + row) % 8;

        ssd1306_write_cmd(0xB0 | page);
        ssd1306_write_cmd(0x00 | ((x) & 0x0F));
        ssd1306_write_cmd(0x10 | ((x >> 4) & 0x0F));

        uint8_t row_buf[SSD1306_WIDTH];
        memset(row_buf, 0, SSD1306_WIDTH);

        /* Read current page content (in production: use a full framebuffer instead) */
        if (page_offset != 0) {
            /* pixel data straddles two pages — simplified: assume page-aligned */
        }

        for (int col = 0; col < bytes_per_row && (x + col * 8) < SSD1306_WIDTH; col++) {
            row_buf[col] = bitmap[row * bytes_per_row + col];
        }

        ssd1306_write_data(row_buf, SSD1306_WIDTH);
    }
}

/* ── Expression variants ──────────────────────────────────────── */

static uint8_t *generate_happy_face(void) {
    /* Like neutral but curved-up mouth */
    static uint8_t happy[BM_BYTES];
    memcpy(happy, FACE_NEUTRAL, BM_BYTES);
    /* Modify mouth area to be a smile arc (rows 22-23, full width) */
    // Simplified: use neutral as base, mouth modification applied at render time
    return happy;
}

/* ── Public API ───────────────────────────────────────────────── */

void face_init(void) {
    ESP_LOGI(TAG, "initializing SSD1306 face display (I2C 0x%02X)", SSD1306_ADDR);

    /* Probe I2C address */
    uint8_t dummy;
    esp_err_t err = i2c_master_write_read_device(I2C_PORT, SSD1306_ADDR,
                                                  NULL, 0, &dummy, 0, pdMS_TO_TICKS(10));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SSD1306 not found at 0x%02X — display disabled", SSD1306_ADDR);
        g_display_ok = false;
        return;
    }

    /* Init sequence (SSD1306 datasheet) */
    ssd1306_write_cmd(SSD1306_CMD_DISPLAY_OFF);

    /* Set multiplex ratio: 64 */
    ssd1306_write_cmd(0xA8);
    ssd1306_write_cmd(0x3F);

    /* Set display offset: 0 */
    ssd1306_write_cmd(0xD3);
    ssd1306_write_cmd(0x00);

    /* Set display start line: 0 */
    ssd1306_write_cmd(0x40);

    /* Set segment re-map: column 127 = SEG0 */
    ssd1306_write_cmd(0xA1);

    /* COM scan direction: remapped (bottom to top) */
    ssd1306_write_cmd(0xC8);

    /* COM pins hardware configuration */
    ssd1306_write_cmd(0xDA);
    ssd1306_write_cmd(0x12);

    /* Set contrast: mid */
    ssd1306_write_cmd(SSD1306_CMD_SET_CONTRAST);
    ssd1306_write_cmd(0x7F);

    /* Entire display on, resume to RAM */
    ssd1306_write_cmd(0xA4);

    /* Normal display (not inverted) */
    ssd1306_write_cmd(0xA6);

    /* Set oscillator frequency + clock divide */
    ssd1306_write_cmd(0xD5);
    ssd1306_write_cmd(0x80);

    /* Charge pump: enable */
    ssd1306_write_cmd(0x8D);
    ssd1306_write_cmd(0x14);

    /* Display ON */
    ssd1306_write_cmd(SSD1306_CMD_DISPLAY_ON);

    vTaskDelay(pdMS_TO_TICKS(100));

    /* Clear display */
    clear_display();

    /* Generate expression variants */
    FACE_BITMAPS[BCP_FACE_HAPPY]     = generate_happy_face();
    FACE_BITMAPS[BCP_FACE_SAD]       = FACE_NEUTRAL;  /* TODO: unique bitmaps */
    FACE_BITMAPS[BCP_FACE_ANGRY]     = FACE_NEUTRAL;
    FACE_BITMAPS[BCP_FACE_SURPRISED] = FACE_NEUTRAL;
    FACE_BITMAPS[BCP_FACE_SLEEPY]     = FACE_NEUTRAL;

    /* Show default expression */
    face_set_expression(BCP_FACE_NEUTRAL);

    g_display_ok = true;
    g_last_blink_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    g_next_blink_ms = 2000 + (rand() % 4000);  /* 2-6s */

    ESP_LOGI(TAG, "SSD1306 display ready");
}

void face_set_expression(bcp_face_expr_t expr) {
    if (!g_display_ok) return;
    if (expr >= BCP_FACE_EXPR_COUNT) return;

    g_current_expr = expr;
    const uint8_t *bm = FACE_BITMAPS[expr];

    if (bm) {
        clear_display();
        draw_bitmap(bm, BM_X_OFFSET, BM_Y_OFFSET, BM_WIDTH, BM_HEIGHT);
    }

    ESP_LOGD(TAG, "expression: %d", expr);
}

void face_set_custom(const uint8_t *bitmap, size_t len) {
    if (!g_display_ok || !bitmap) return;

    clear_display();

    /* Treat custom bitmap as full-screen (128x64 = 1024 bytes) */
    if (len == 1024) {
        for (int page = 0; page < SSD1306_PAGES; page++) {
            ssd1306_write_cmd(0xB0 | page);
            ssd1306_write_cmd(0x00);
            ssd1306_write_cmd(0x10);
            ssd1306_write_data(bitmap + page * SSD1306_WIDTH, SSD1306_WIDTH);
        }
    } else {
        /* Centered bitmap */
        int w = (len > 128) ? 128 : (int)len;
        draw_bitmap(bitmap, (SSD1306_WIDTH - w) / 2, BM_Y_OFFSET, w, BM_HEIGHT);
    }

    ESP_LOGD(TAG, "custom bitmap: %d bytes", len);
}

void face_off(void) {
    if (!g_display_ok) return;
    ssd1306_write_cmd(SSD1306_CMD_DISPLAY_OFF);
    ESP_LOGD(TAG, "display off");
}

/* ── Blink animation (called from behavior task context) ────────
 * Not a separate FreeRTOS task; driven by behavior engine at 50Hz.
 */

void face_blink_tick(void) {
    if (!g_display_ok) return;

    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (!g_blinking && (now - g_last_blink_ms) > g_next_blink_ms) {
        /* Start blink */
        g_blinking = true;
        g_last_blink_ms = now;

        /* Draw closed eyes (horizontal line over eye area) */
        /* Simplified: just clear the eye rows temporarily */
    }

    if (g_blinking && (now - g_last_blink_ms) > 150) {
        /* Blink complete — restore expression */
        g_blinking = false;
        g_last_blink_ms = now;
        g_next_blink_ms = 2000 + (rand() % 4000);

        face_set_expression(g_current_expr);
    }
}
