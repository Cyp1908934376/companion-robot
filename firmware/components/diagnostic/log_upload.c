/**
 * Remote logging — ring buffer + BCP upload for error/warn events.
 */

#include "log_upload.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static const char *TAG = "log_upload";

/* ── Ring buffer ─────────────────────────────────────────────── */

static log_entry_t g_log_ring[LOG_RING_SIZE];
static int g_log_head = 0;   /* next write position */
static int g_log_tail = 0;   /* next read position */
static int g_log_count = 0;

extern QueueHandle_t q_cmd_outgoing;

/* ── Public API ──────────────────────────────────────────────── */

void log_upload_init(void) {
    memset(g_log_ring, 0, sizeof(g_log_ring));
    g_log_head  = 0;
    g_log_tail  = 0;
    g_log_count = 0;
    ESP_LOGI(TAG, "log upload ready (ring=%d entries)", LOG_RING_SIZE);
}

void log_event(log_level_t level, const char *tag, const char *fmt, ...) {
    /* Format message */
    char msg[LOG_MSG_MAX_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    /* Build log entry */
    log_entry_t entry;
    entry.level        = level;
    entry.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    strncpy(entry.tag, tag ? tag : "?", sizeof(entry.tag) - 1);
    entry.tag[sizeof(entry.tag) - 1] = '\0';
    strncpy(entry.message, msg, sizeof(entry.message) - 1);
    entry.message[sizeof(entry.message) - 1] = '\0';

    /* ERROR: immediate upload via BCP_LOG_EVENT */
    if (level == LOG_LEVEL_ERROR) {
        bcp_frame_t frame;
        bcp_frame_init(&frame, 0);  /* seq_no set by comm_tx */

        bcp_command_t cmd;
        cmd.cmd_id      = BCP_CMD_LOG_EVENT;
        cmd.payload_len = 2;  /* level(1) + tag_prefix(1) */
        cmd.payload.raw[0] = (uint8_t)level;
        cmd.payload.raw[1] = (uint8_t)(tag[0] ? tag[0] : '?');

        bcp_frame_push(&frame, &cmd);
        xQueueSend(q_cmd_outgoing, &frame, 0);
    }

    /* Store in ring buffer */
    g_log_ring[g_log_head] = entry;
    g_log_head = (g_log_head + 1) % LOG_RING_SIZE;
    if (g_log_count < LOG_RING_SIZE) {
        g_log_count++;
    } else {
        g_log_tail = (g_log_tail + 1) % LOG_RING_SIZE;  /* overwrite oldest */
    }
}

int log_pending_count(void) {
    return g_log_count;
}

bool log_pop_pending(log_entry_t *out) {
    if (g_log_count == 0) return false;
    *out = g_log_ring[g_log_tail];
    g_log_tail = (g_log_tail + 1) % LOG_RING_SIZE;
    g_log_count--;
    return true;
}

int log_upload_response(bcp_frame_t *frame, uint16_t seq_no) {
    int count = 0;

    /* Drain ring buffer into frame (up to BCP_MAX_CMDS_PER_FRAME) */
    log_entry_t entry;
    while (log_pop_pending(&entry) && count < BCP_MAX_CMDS_PER_FRAME) {
        bcp_command_t cmd;
        cmd.cmd_id      = BCP_CMD_LOG_EVENT;
        /* Payload: [level(1)][timestamp(4)][tag_len(1)][tag(0-15)][msg(0-N)] */
        int tag_len = (int)strlen(entry.tag);
        if (tag_len > 15) tag_len = 15;
        int msg_len = (int)strlen(entry.message);
        int max_msg = BCP_MAX_PAYLOAD_LEN - 6 - tag_len;
        if (msg_len > max_msg) msg_len = max_msg;

        int off = 0;
        cmd.payload.raw[off++] = (uint8_t)entry.level;
        cmd.payload.raw[off++] = (uint8_t)(entry.timestamp_ms & 0xFF);
        cmd.payload.raw[off++] = (uint8_t)((entry.timestamp_ms >> 8) & 0xFF);
        cmd.payload.raw[off++] = (uint8_t)((entry.timestamp_ms >> 16) & 0xFF);
        cmd.payload.raw[off++] = (uint8_t)((entry.timestamp_ms >> 24) & 0xFF);
        cmd.payload.raw[off++] = (uint8_t)tag_len;
        memcpy(&cmd.payload.raw[off], entry.tag, tag_len);
        off += tag_len;
        memcpy(&cmd.payload.raw[off], entry.message, msg_len);
        off += msg_len;
        cmd.payload_len = (uint8_t)off;

        bcp_frame_push(frame, &cmd);
        count++;
    }

    return count;
}

int diag_response(bcp_frame_t *frame, uint8_t diag_type) {
    bcp_command_t cmd;
    cmd.cmd_id = BCP_CMD_DIAG_RESP;

    switch (diag_type) {
    case 0x01: {  /* DIAG_MEM: memory usage */
        /* Report free heap */
        uint32_t free_heap = esp_get_free_heap_size();
        uint32_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        cmd.payload_len = 8;
        cmd.payload.raw[0] = diag_type;
        cmd.payload.raw[1] = (uint8_t)(free_heap & 0xFF);
        cmd.payload.raw[2] = (uint8_t)((free_heap >> 8) & 0xFF);
        cmd.payload.raw[3] = (uint8_t)((free_heap >> 16) & 0xFF);
        cmd.payload.raw[4] = (uint8_t)((free_heap >> 24) & 0xFF);
        cmd.payload.raw[5] = (uint8_t)(free_psram & 0xFF);
        cmd.payload.raw[6] = (uint8_t)((free_psram >> 8) & 0xFF);
        cmd.payload.raw[7] = (uint8_t)((free_psram >> 16) & 0xFF);
        break;
    }
    case 0x02: {  /* DIAG_TASK: task count */
        /* Report FreeRTOS task count */
        uint32_t task_count = (uint32_t)uxTaskGetNumberOfTasks();
        cmd.payload_len = 5;
        cmd.payload.raw[0] = diag_type;
        cmd.payload.raw[1] = (uint8_t)(task_count & 0xFF);
        cmd.payload.raw[2] = (uint8_t)((task_count >> 8) & 0xFF);
        cmd.payload.raw[3] = (uint8_t)LOG_RING_SIZE;
        cmd.payload.raw[4] = (uint8_t)g_log_count;
        break;
    }
    case 0x03: {  /* DIAG_SENSOR: sensor self-check bitmap */
        uint8_t sensor_ok = 0;
        /* Bitmap: bit0=IMU, bit1=ToF, bit2=Env, bit3=Touch, bit4=Battery */
        /* Simplistic: assume all OK during boot, self-tests fill in later */
        cmd.payload_len = 2;
        cmd.payload.raw[0] = diag_type;
        cmd.payload.raw[1] = sensor_ok;
        break;
    }
    case 0x04: {  /* DIAG_PING: echo back */
        cmd.payload_len = 1;
        cmd.payload.raw[0] = diag_type;
        break;
    }
    default:
        cmd.payload_len = 2;
        cmd.payload.raw[0] = diag_type;
        cmd.payload.raw[1] = 0xFF;  /* unknown diag */
        break;
    }

    bcp_frame_push(frame, &cmd);
    return 1;
}
