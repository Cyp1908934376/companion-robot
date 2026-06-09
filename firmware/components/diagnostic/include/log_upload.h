/**
 * Remote logging — error/warn capture and upload to main brain.
 *
 * Ring buffer for log events (up to 64 entries). ERROR events trigger
 * immediate upload via BCP_CMD_LOG_EVENT. WARN/INFO events are buffered
 * and uploaded on request via BCP_CMD_LOG_UPLOAD.
 *
 * Also handles basic system diagnostics (memory, task stats).
 */

#ifndef LOG_UPLOAD_H
#define LOG_UPLOAD_H

#include "bcp_codec.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Configuration ─────────────────────────────────────────── */

#define LOG_RING_SIZE        64     /* max buffered log entries */
#define LOG_MSG_MAX_LEN      128    /* max message length per entry */

/* ── Types ──────────────────────────────────────────────────── */

typedef enum {
    LOG_LEVEL_ERROR = 1,
    LOG_LEVEL_WARN  = 2,
    LOG_LEVEL_INFO  = 3,
    LOG_LEVEL_DEBUG = 4,
} log_level_t;

typedef struct {
    log_level_t level;
    uint32_t    timestamp_ms;
    char        tag[16];
    char        message[LOG_MSG_MAX_LEN];
} log_entry_t;

/* ── API ────────────────────────────────────────────────────── */

/** Initialize the log upload subsystem. */
void log_upload_init(void);

/** Record a log event. ERROR events trigger immediate BCP upload. */
void log_event(log_level_t level, const char *tag, const char *fmt, ...);

/** Get count of pending (non-ERROR) log entries. */
int log_pending_count(void);

/** Pop the oldest pending log entry. Returns false if buffer empty. */
bool log_pop_pending(log_entry_t *out);

/** Handle BCP_CMD_LOG_UPLOAD: encode response frame with pending logs. */
int log_upload_response(bcp_frame_t *frame, uint16_t seq_no);

/** Handle BCP_CMD_DIAG_REQ: encode diagnostic response.
 *  Returns number of commands added to frame. */
int diag_response(bcp_frame_t *frame, uint8_t diag_type);

#ifdef __cplusplus
}
#endif

#endif /* LOG_UPLOAD_H */
