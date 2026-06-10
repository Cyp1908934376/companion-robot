/// WiFi reporter — connects to main brain and reports station status.
///
/// Protocol:
///   - Connects to the gateway via WebSocket (same as robots)
///   - Sends periodic status messages: { station_id, state, v_out, i_out, robot_id }
///   - Receives commands: enable/disable beacon, force charge off
///
/// Uses a simple JSON-over-TCP protocol (lighter than full BCP for station).

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_DISCONNECTED,
    WIFI_CONNECTING,
    WIFI_CONNECTED,
} wifi_state_t;

typedef struct {
    char        ssid[33];
    char        password[65];
    char        gateway_host[64];
    uint16_t    gateway_port;
    uint8_t     station_id;
    wifi_state_t wifi_state;
    int         sock_fd;
    bool        report_enabled;
} wifi_reporter_t;

/// Event pushed to the reporter task from the main loop.
typedef struct {
    uint8_t  station_id;
    uint8_t  charge_state;   // charge_state_t cast
    float    v_out;
    float    i_out;
    uint32_t uptime_s;
} reporter_event_t;

/// Initialize WiFi and connect to the gateway.
void wifi_reporter_init(wifi_reporter_t *rep,
                        const char *ssid, const char *password,
                        const char *gateway_host, uint16_t gateway_port,
                        uint8_t station_id);

/// Start the background reporting task.
/// Returns the queue handle for pushing status events.
QueueHandle_t wifi_reporter_start(wifi_reporter_t *rep);

/// Push a status update (non-blocking, called from main loop).
bool wifi_reporter_report(QueueHandle_t queue, const reporter_event_t *ev);

#ifdef __cplusplus
}
#endif
