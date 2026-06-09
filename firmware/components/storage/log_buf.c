/**
 * Log ring buffer implementation.
 *
 * Ring buffer in PSRAM with structured entries.
 * Thread-safe for concurrent writes from multiple tasks.
 */

#include "log_buf.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

static const char *TAG = "log_buf";

/* ── Buffer state ─────────────────────────────────────────────── */

#define DEFAULT_BUF_SIZE    32768   /* 32KB = ~2000 typical entries */
#define ENTRY_OVERHEAD       8      /* timestamp(4) + level(1) + tag_len(1) + msg_len(2) */

static uint8_t *g_buf;
static size_t   g_buf_size;
static size_t   g_write_pos;   /* next write position */
static size_t   g_entry_count; /* total entries written */
static portMUX_TYPE g_spinlock = portMUX_INITIALIZER_UNLOCKED;

/* ── Encoding helpers ─────────────────────────────────────────── */

/**
 * Encode a log entry into the ring buffer at the current write position.
 * Returns the number of bytes written (including overhead).
 */
static size_t encode_entry(uint8_t *dest, size_t dest_cap,
                           log_level_t level, const char *tag,
                           const char *msg) {
    size_t tag_len = strlen(tag);
    size_t msg_len = strlen(msg);

    if (tag_len > LOG_TAG_MAX_LEN - 1) tag_len = LOG_TAG_MAX_LEN - 1;
    if (msg_len > LOG_MSG_MAX_LEN - 1) msg_len = LOG_MSG_MAX_LEN - 1;

    size_t total = ENTRY_OVERHEAD + tag_len + msg_len;
    if (total > dest_cap) return 0;

    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    /* timestamp_ms (4B) */
    dest[0] = (now >> 0)  & 0xFF;
    dest[1] = (now >> 8)  & 0xFF;
    dest[2] = (now >> 16) & 0xFF;
    dest[3] = (now >> 24) & 0xFF;

    /* level (1B) */
    dest[4] = (uint8_t)level;

    /* tag_len (1B) + tag */
    dest[5] = (uint8_t)tag_len;
    memcpy(dest + 6, tag, tag_len);

    /* msg_len (2B LE) + msg */
    dest[6 + tag_len] = (uint8_t)(msg_len & 0xFF);
    dest[7 + tag_len] = (uint8_t)((msg_len >> 8) & 0xFF);
    memcpy(dest + 8 + tag_len, msg, msg_len);

    return total;
}

/**
 * Decode one entry from buffer at position. Advances *pos past the entry.
 * Returns true on success.
 */
static bool decode_entry(const uint8_t *buf, size_t buf_size, size_t *pos,
                         log_entry_t *out) {
    if (*pos + ENTRY_OVERHEAD > buf_size) return false;

    out->timestamp_ms = (uint32_t)buf[*pos]
                      | ((uint32_t)buf[*pos + 1] << 8)
                      | ((uint32_t)buf[*pos + 2] << 16)
                      | ((uint32_t)buf[*pos + 3] << 24);
    out->level = (log_level_t)buf[*pos + 4];

    size_t tag_len = buf[*pos + 5];
    if (tag_len >= LOG_TAG_MAX_LEN) tag_len = LOG_TAG_MAX_LEN - 1;

    size_t msg_offset = *pos + 6 + tag_len;
    if (msg_offset + 2 > buf_size) return false;

    size_t msg_len = (size_t)buf[msg_offset]
                   | ((size_t)buf[msg_offset + 1] << 8);
    if (msg_len >= LOG_MSG_MAX_LEN) msg_len = LOG_MSG_MAX_LEN - 1;

    if (msg_offset + 2 + msg_len > buf_size) return false;

    memcpy(out->tag, buf + *pos + 6, tag_len);
    out->tag[tag_len] = '\0';

    memcpy(out->msg, buf + msg_offset + 2, msg_len);
    out->msg[msg_len] = '\0';

    *pos += ENTRY_OVERHEAD + tag_len + msg_len;
    return true;
}

/* ── Public API ───────────────────────────────────────────────── */

void log_buf_init(size_t buffer_size) {
    if (buffer_size == 0) buffer_size = DEFAULT_BUF_SIZE;

    g_buf = heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_buf) {
        /* Fallback to internal DRAM */
        g_buf = malloc(buffer_size);
        ESP_LOGW(TAG, "PSRAM alloc failed — using internal DRAM");
    }

    if (!g_buf) {
        ESP_LOGE(TAG, "log buffer allocation failed!");
        g_buf_size = 0;
        return;
    }

    g_buf_size    = buffer_size;
    g_write_pos   = 0;
    g_entry_count = 0;

    memset(g_buf, 0, g_buf_size);
    ESP_LOGI(TAG, "log buffer ready (%d bytes)", g_buf_size);
}

void log_buf_append(log_level_t level, const char *tag, const char *fmt, ...) {
    if (!g_buf || g_buf_size == 0) return;

    /* Format message */
    char msg[LOG_MSG_MAX_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, LOG_MSG_MAX_LEN, fmt, args);
    va_end(args);

    portENTER_CRITICAL(&g_spinlock);

    size_t entry_size = encode_entry(NULL, 0, level, tag, msg);  /* dry-run for size */
    /* Recalculate with actual tag_len + msg_len */
    size_t tag_len = strlen(tag);
    size_t msg_len = strlen(msg);
    if (tag_len > LOG_TAG_MAX_LEN - 1) tag_len = LOG_TAG_MAX_LEN - 1;
    if (msg_len > LOG_MSG_MAX_LEN - 1) msg_len = LOG_MSG_MAX_LEN - 1;
    entry_size = ENTRY_OVERHEAD + tag_len + msg_len;

    /* Wrap-around check */
    if (g_write_pos + entry_size > g_buf_size) {
        /* Wrap to beginning (overwrites old entries) */
        g_write_pos = 0;
    }

    size_t written = encode_entry(g_buf + g_write_pos,
                                  g_buf_size - g_write_pos,
                                  level, tag, msg);
    g_write_pos += written;
    g_entry_count++;

    portEXIT_CRITICAL(&g_spinlock);
}

size_t log_buf_query(log_level_t level, size_t limit, log_entry_t *out) {
    if (!g_buf || !out || limit == 0) return 0;

    portENTER_CRITICAL(&g_spinlock);

    size_t found = 0;
    size_t pos = 0;

    while (pos < g_buf_size && found < limit) {
        log_entry_t entry;
        size_t prev_pos = pos;
        if (!decode_entry(g_buf, g_buf_size, &pos, &entry)) {
            break;  /* corrupt entry or end of data */
        }
        if (entry.level <= level) {
            out[found++] = entry;
        }
    }

    portEXIT_CRITICAL(&g_spinlock);
    return found;
}

size_t log_buf_encode_recent(uint8_t *buf, size_t buf_len) {
    /* Return WARN+ entries from the last check. Not implemented as delta —
     * returns up to 256 bytes of recent WARN/ERROR entries. */
    if (!g_buf || !buf || buf_len < 4) return 0;

    size_t written = 0;

    /* Count of entries to follow */
    size_t count = 0;
    size_t count_pos = written;
    written += 2;  /* reserve 2 bytes for count */

    log_entry_t entries[8];
    size_t n = log_buf_query(LOG_LEVEL_WARN, 8, entries);

    for (size_t i = 0; i < n && written + 64 < buf_len; i++) {
        /* Level(1B) + tag(up to 16) + msg(up to 48) */
        buf[written++] = (uint8_t)entries[i].level;
        size_t tag_len = strlen(entries[i].tag);
        buf[written++] = (uint8_t)tag_len;
        memcpy(buf + written, entries[i].tag, tag_len);
        written += (size_t)tag_len;
        size_t msg_len = strlen(entries[i].msg);
        if (msg_len > 48) msg_len = 48;
        buf[written++] = (uint8_t)msg_len;
        memcpy(buf + written, entries[i].msg, msg_len);
        written += msg_len;
        count++;
    }

    if (count > 0) {
        buf[count_pos]     = (uint8_t)(count & 0xFF);
        buf[count_pos + 1] = (uint8_t)((count >> 8) & 0xFF);
    } else {
        written = 0;  /* nothing to report */
    }

    return written;
}

size_t log_buf_encode_upload(uint8_t *buf, size_t buf_len, log_level_t level) {
    if (!g_buf || !buf || buf_len < 2) return 0;

    size_t written = 2;  /* reserve for count */

    log_entry_t entries[32];
    size_t n = log_buf_query(level, 32, entries);

    for (size_t i = 0; i < n && written + 80 < buf_len; i++) {
        /* timestamp(4B) + level(1B) + tag_len(1B) + tag + msg_len(2B) + msg */
        log_entry_t *e = &entries[i];

        uint32_t ts = e->timestamp_ms;
        buf[written++] = (uint8_t)(ts & 0xFF);
        buf[written++] = (uint8_t)((ts >> 8) & 0xFF);
        buf[written++] = (uint8_t)((ts >> 16) & 0xFF);
        buf[written++] = (uint8_t)((ts >> 24) & 0xFF);

        buf[written++] = (uint8_t)e->level;

        size_t tag_len = strlen(e->tag);
        buf[written++] = (uint8_t)tag_len;
        memcpy(buf + written, e->tag, tag_len);
        written += tag_len;

        size_t msg_len = strlen(e->msg);
        buf[written++] = (uint8_t)(msg_len & 0xFF);
        buf[written++] = (uint8_t)((msg_len >> 8) & 0xFF);
        memcpy(buf + written, e->msg, msg_len);
        written += msg_len;
    }

    if (written > 2) {
        buf[0] = (uint8_t)(written & 0xFF);
        buf[1] = (uint8_t)((written >> 8) & 0xFF);
    } else {
        written = 0;
    }

    return written;
}

size_t log_buf_count(void) {
    return g_entry_count;
}

void log_buf_clear(void) {
    portENTER_CRITICAL(&g_spinlock);
    memset(g_buf, 0, g_buf_size);
    g_write_pos = 0;
    g_entry_count = 0;
    portEXIT_CRITICAL(&g_spinlock);
}
