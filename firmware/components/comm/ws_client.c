/**
 * WebSocket client — BCP frame transport over WiFi.
 *
 * Implements:
 *   - WebSocket connect/handshake to gateway
 *   - BCP frame send (encode → binary WS frame → TCP)
 *   - BCP frame receive (TCP → binary WS frame → decode → dispatch)
 */

#include "ws_client.h"
#include "conn_manager.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_http_client.h"
#include <string.h>

static const char *TAG = "ws_client";

ws_client_t g_ws_client = {
    .sock_fd = -1,
    .connected = false,
    .rx_buf_len = 0,
};

/* External queues — defined in main.c */
extern QueueHandle_t q_cmd_incoming;
extern QueueHandle_t q_cmd_outgoing;

/* WebSocket frame types */
#define WS_OPCODE_TEXT   0x01
#define WS_OPCODE_BINARY 0x02
#define WS_OPCODE_CLOSE  0x08
#define WS_OPCODE_PING   0x09
#define WS_OPCODE_PONG   0x0A

int ws_connect(const char *host, uint16_t port) {
    /* In production, use WebSocket handshake + TLS.
     * For now, this is a stub that would integrate with esp_websocket_client.
     */
    ESP_LOGI(TAG, "connecting to ws://%s:%d", host, port);
    g_ws_client.connected = true;
    g_ws_client.rx_buf_len = 0;
    xEventGroupSetBits(evg_system, EVG_WS_CONNECTED);
    return 0;
}

void ws_disconnect(void) {
    if (g_ws_client.connected) {
        g_ws_client.connected = false;
        g_ws_client.sock_fd = -1;
        xEventGroupClearBits(evg_system, EVG_WS_CONNECTED);
        ESP_LOGI(TAG, "disconnected from gateway");
    }
}

int ws_send_raw(const uint8_t *data, size_t len) {
    if (!g_ws_client.connected) return -1;

    /* In production: send binary WebSocket frame with masking.
     * Simplified stub: directly write to socket (TCP mode).
     */
    ESP_LOGD(TAG, "send_raw: %d bytes", len);
    return (int)len;
}

int ws_send_frame(const bcp_frame_t *frame) {
    uint8_t buf[BCP_MAX_FRAME_LEN];
    int len = bcp_encode(frame, buf, sizeof(buf));
    if (len < 0) {
        ESP_LOGE(TAG, "BCP encode failed: %s", bcp_error_str((bcp_error_t)len));
        return -1;
    }
    return ws_send_raw(buf, (size_t)len);
}

/* ── Communication tasks ──────────────────────────────────── */

void task_comm_rx(void *arg) {
    ESP_LOGI(TAG, "comm_rx task started");

    while (1) {
        /* In production: read from WebSocket/TCP socket into rx_buf.
         * Parse individual BCP frames and dispatch commands.
         *
         * Simplified: poll a queue for test/demo frames.
         */
        bcp_frame_t frame;
        if (xQueueReceive(q_cmd_incoming, &frame, pdMS_TO_TICKS(100)) == pdTRUE) {
            /* Frame received — in production this comes from the network.
             * Dispatch each command to the appropriate handler task
             * via internal queues.
             */
            for (int i = 0; i < frame.cmd_count; i++) {
                bcp_command_t cmd = frame.commands[i];
                switch (cmd.cmd_id) {
                case BCP_CMD_MOVE:
                case BCP_CMD_MOVE_TO:
                case BCP_CMD_STOP:
                case BCP_CMD_SERVO_SET:
                case BCP_CMD_HEAD_PAN_TILT:
                    /* Forward to motor/servo task */
                    break;
                case BCP_CMD_LED_SET:
                case BCP_CMD_LED_PATTERN:
                case BCP_CMD_LED_OFF:
                    /* Forward to LED task */
                    break;
                case BCP_CMD_FACE_EXPR:
                case BCP_CMD_FACE_CUSTOM:
                    /* Forward to face/expression task */
                    break;
                case BCP_CMD_SPEAK:
                case BCP_CMD_TTS_TEXT:
                    /* Forward to audio output task */
                    break;
                case BCP_CMD_RESET:
                    ESP_LOGW(TAG, "RESET command received, rebooting...");
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_restart();
                    break;
                default:
                    break;
                }
            }
        }

        esp_task_wdt_reset();
    }
}

void task_comm_tx(void *arg) {
    ESP_LOGI(TAG, "comm_tx task started");

    while (1) {
        bcp_frame_t frame;
        if (xQueueReceive(q_cmd_outgoing, &frame, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (g_ws_client.connected) {
                ws_send_frame(&frame);
            }
            /* If not connected via WiFi, try BLE */
            if (!g_ws_client.connected && g_ble.connected) {
                ble_send_frame(&frame);
            }
        }

        esp_task_wdt_reset();
    }
}
