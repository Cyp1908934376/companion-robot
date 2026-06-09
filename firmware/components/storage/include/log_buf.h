/**
 * Log ring buffer — structured log storage in PSRAM with remote upload.
 *
 * Log entry format in buffer:
 *   [timestamp_ms(4B)][level(1B)][tag_len(1B)][tag(NB)][msg_len(2B)][msg(NB)]
 *
 * ERROR logs auto-uploaded immediately via BCP.
 * WARN logs batched in heartbeat.
 * INFO/DEBUG only uploaded on request (BCP_CMD_LOG_UPLOAD).
 */

#ifndef LOG_BUF_H
#define LOG_BUF_H

#include "freertos/FreeRTOS.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Log levels (matching ESP_LOG). */
typedef enum {
    LOG_LEVEL_NONE = 0,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_WARN,
    LOG_LEVEL_INFO,
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_VERBOSE,
} log_level_t;

/** Maximum tag string length. */
#define LOG_TAG_MAX_LEN     16
#define LOG_MSG_MAX_LEN     128

/** Single log entry (decoded). */
typedef struct {
    uint32_t    timestamp_ms;
    log_level_t level;
    char        tag[LOG_TAG_MAX_LEN];
    char        msg[LOG_MSG_MAX_LEN];
} log_entry_t;

/** Initialize log ring buffer in PSRAM.
 *  @param buffer_size  size in bytes (default 32768)
 */
void log_buf_init(size_t buffer_size);

/** Append a log entry to the ring buffer.
 *  @param level       severity level
 *  @param tag         subsystem tag (max 15 chars)
 *  @param fmt         printf-style format string
 *  @param ...         format arguments
 */
void log_buf_append(log_level_t level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/** Query recent log entries matching level filter.
 *  @param level    minimum level (inclusive)
 *  @param limit    max entries to return
 *  @param out      output array (caller allocates)
 *  @return         number of entries returned
 */
size_t log_buf_query(log_level_t level, size_t limit, log_entry_t *out);

/** Encode recent WARN+ entries into a buffer for heartbeat piggyback.
 *  @param buf      output buffer
 *  @param buf_len  buffer capacity
 *  @return         bytes written (0 if nothing to send)
 */
size_t log_buf_encode_recent(uint8_t *buf, size_t buf_len);

/** Encode entries matching level filter into a full upload BCP payload.
 *  @param buf      output buffer
 *  @param buf_len  buffer capacity
 *  @param level    minimum level
 *  @return         bytes written
 */
size_t log_buf_encode_upload(uint8_t *buf, size_t buf_len, log_level_t level);

/** Get number of entries currently buffered. */
size_t log_buf_count(void);

/** Clear the log buffer. */
void log_buf_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* LOG_BUF_H */
