#include "wifi_reporter.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdio.h>

#define TAG "wifi_reporter"

#define REPORT_INTERVAL_MS  5000
#define RECONNECT_DELAY_MS  10000
#define QUEUE_DEPTH         8

// ── WiFi event handler ─────────────────────────────────────────

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    wifi_reporter_t *rep = (wifi_reporter_t *)arg;

    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            rep->wifi_state = WIFI_CONNECTING;
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            rep->wifi_state = WIFI_DISCONNECTED;
            ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
            esp_wifi_connect();
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        rep->wifi_state = WIFI_CONNECTED;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&ev->ip_info.ip));
    }
}

void wifi_reporter_init(wifi_reporter_t *rep,
                        const char *ssid, const char *password,
                        const char *gateway_host, uint16_t gateway_port,
                        uint8_t station_id)
{
    strncpy(rep->ssid, ssid, sizeof(rep->ssid) - 1);
    strncpy(rep->password, password, sizeof(rep->password) - 1);
    strncpy(rep->gateway_host, gateway_host, sizeof(rep->gateway_host) - 1);
    rep->gateway_port   = gateway_port;
    rep->station_id     = station_id;
    rep->wifi_state     = WIFI_DISCONNECTED;
    rep->sock_fd        = -1;
    rep->report_enabled = false;

    // Init NVS (required by WiFi)
    nvs_flash_init();

    // Init TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, rep, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, rep, NULL));

    wifi_config_t wifi_cfg = {0};
    memcpy(wifi_cfg.sta.ssid, ssid, strlen(ssid));
    memcpy(wifi_cfg.sta.password, password, strlen(password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "initialized, connecting to %s", ssid);
}

// ── TCP connection to gateway ───────────────────────────────────

static int connect_to_gateway(const wifi_reporter_t *rep)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(rep->gateway_port),
    };
    inet_pton(AF_INET, rep->gateway_host, &addr.sin_addr);

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// ── JSON report format ─────────────────────────────────────────

static int build_report_json(char *buf, size_t len, const reporter_event_t *ev)
{
    return snprintf(buf, len,
        "{\"type\":\"station_status\","
        "\"station_id\":%u,"
        "\"state\":%u,"
        "\"v_out\":%.2f,"
        "\"i_out\":%.2f,"
        "\"uptime_s\":%lu}\n",
        ev->station_id, ev->charge_state,
        ev->v_out, ev->i_out,
        ev->uptime_s);
}

// ── Background task ─────────────────────────────────────────────

typedef struct {
    wifi_reporter_t *rep;
    QueueHandle_t    queue;
} reporter_task_ctx_t;

static void reporter_task(void *arg)
{
    reporter_task_ctx_t *ctx = (reporter_task_ctx_t *)arg;
    wifi_reporter_t *rep    = ctx->rep;
    QueueHandle_t queue     = ctx->queue;
    reporter_event_t ev;
    uint32_t last_report_ms = 0;

    // Wait for WiFi to connect
    while (rep->wifi_state != WIFI_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "reporter task started");

    while (1) {
        // Connect to gateway if needed
        if (rep->sock_fd < 0) {
            rep->sock_fd = connect_to_gateway(rep);
            if (rep->sock_fd >= 0) {
                rep->report_enabled = true;
                ESP_LOGI(TAG, "connected to gateway %s:%d",
                         rep->gateway_host, rep->gateway_port);
            }
        }

        // Wait for event with timeout
        if (xQueueReceive(queue, &ev, pdMS_TO_TICKS(REPORT_INTERVAL_MS)) == pdTRUE) {
            last_report_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        }

        // Send report
        if (rep->report_enabled && rep->sock_fd >= 0) {
            char json[256];
            int len = build_report_json(json, sizeof(json), &ev);
            int sent = send(rep->sock_fd, json, len, 0);
            if (sent < 0) {
                ESP_LOGW(TAG, "send failed, closing socket");
                close(rep->sock_fd);
                rep->sock_fd = -1;
                rep->report_enabled = false;
            }
        }
    }
}

// ── Public API ───────────────────────────────────────────────────

QueueHandle_t wifi_reporter_start(wifi_reporter_t *rep)
{
    QueueHandle_t queue = xQueueCreate(QUEUE_DEPTH, sizeof(reporter_event_t));

    reporter_task_ctx_t *ctx = malloc(sizeof(reporter_task_ctx_t));
    ctx->rep   = rep;
    ctx->queue = queue;

    xTaskCreate(reporter_task, "wifi_reporter", 4096, ctx, 5, NULL);
    return queue;
}

bool wifi_reporter_report(QueueHandle_t queue, const reporter_event_t *ev)
{
    return xQueueSend(queue, ev, 0) == pdTRUE;
}
