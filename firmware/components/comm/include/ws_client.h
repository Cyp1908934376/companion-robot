/**
 * WebSocket client — connects to the main-brain gateway.
 *
 * Sends/receives BCP frames over a WebSocket connection.
 * Runs on Core 0 as task_comm_rx and task_comm_tx.
 */

#ifndef WS_CLIENT_H
#define WS_CLIENT_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "bcp_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/** WebSocket client context. */
typedef struct {
    int sock_fd;
    bool connected;
    uint8_t rx_buf[BCP_MAX_FRAME_LEN * 2];
    size_t rx_buf_len;
} ws_client_t;

extern ws_client_t g_ws_client;

/** Receive task: reads WebSocket frames, decodes BCP, dispatches commands. */
void task_comm_rx(void *arg);

/** Transmit task: encodes BCP frames from outgoing queue, sends via WebSocket. */
void task_comm_tx(void *arg);

/** Connect to the gateway at the given host:port. Returns 0 on success. */
int ws_connect(const char *host, uint16_t port);

/** Send a raw buffer over WebSocket. */
int ws_send_raw(const uint8_t *data, size_t len);

/** Send a BCP frame over WebSocket. */
int ws_send_frame(const bcp_frame_t *frame);

/** Disconnect from gateway. */
void ws_disconnect(void);

#ifdef __cplusplus
}
#endif

#endif /* WS_CLIENT_H */
