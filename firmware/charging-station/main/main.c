/// Charging station firmware — ESP32-C3.
///
/// Components:
///   - IR beacon: 3-channel 38kHz PPM beacon for robot approach guidance
///   - Charge control: docking detection, charging FSM, overcurrent protection
///   - Status indicator: RGB LED for visual state feedback
///   - WiFi reporter: reports station status to main brain gateway
///
/// Startup sequence:
///   1. Initialize NVS, default event loop
///   2. Configure GPIOs (IR LEDs, power MOSFET, ADC, RGB LED)
///   3. Initialize IR beacon → start broadcasting
///   4. Initialize charge controller → start monitoring
///   5. Initialize status indicator → idle (green)
///   6. Connect WiFi → start reporter task → status → ready (green)

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "ir_beacon.h"
#include "charge_ctl.h"
#include "status_indicator.h"
#include "wifi_reporter.h"

#define TAG "main"

// ── Hardware pin assignments ───────────────────────────────────

#define GPIO_IR_A       0   // IR LED channel A (0° phase)
#define GPIO_IR_B       1   // IR LED channel B (120° phase)
#define GPIO_IR_C       2   // IR LED channel C (240° phase)
#define GPIO_PWR_EN     4   // Charge MOSFET gate
#define GPIO_VSENSE     3   // ADC1_CH0: voltage sense (via divider)
#define GPIO_ISENSE     3   // ADC1_CH0: shared ADC pin (reused)
#define GPIO_RGB_R      5   // Status LED red
#define GPIO_RGB_G      6   // Status LED green
#define GPIO_RGB_B      7   // Status LED blue

// ── Default config (override via sdkconfig or NVS) ─────────────

#define STATION_ID          1
#define WIFI_SSID           "CompanionNet"
#define WIFI_PASS           ""
#define GATEWAY_HOST        "192.168.1.100"
#define GATEWAY_PORT        8080

// ── Globals ────────────────────────────────────────────────────

static ir_beacon_t          g_beacon;
static charge_ctl_t         g_charge;
static status_indicator_t   g_indicator;
static wifi_reporter_t      g_reporter;
static QueueHandle_t        g_report_queue;

// ── Task: status LED blink logic ───────────────────────────────

static void led_blink_task(void *arg)
{
    bool blink_on = false;
    while (1) {
        charge_state_t cs = charge_ctl_get_state(&g_charge);

        if (cs == CHARGE_COMPLETE) {
            // Green blink: on 500ms, off 500ms
            blink_on = !blink_on;
            status_indicator_set_rgb(&g_indicator,
                blink_on ? 0 : 0,
                blink_on ? 255 : 0,
                0);
            vTaskDelay(pdMS_TO_TICKS(500));
        } else if (cs == CHARGE_FAULT) {
            // Red blink: on 200ms, off 200ms
            blink_on = !blink_on;
            status_indicator_set_rgb(&g_indicator,
                blink_on ? 255 : 0,
                0, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// ── main ────────────────────────────────────────────────────────

void app_main(void)
{
    ESP_LOGI(TAG, "=== Charging Station Firmware ===");
    ESP_LOGI(TAG, "Station ID: %d", STATION_ID);
    ESP_LOGI(TAG, "ESP32-C3, IDF %s", esp_get_idf_version());

    // ── 1. Init NVS ──
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // ── 2. Init hardware components ──
    ir_beacon_init(&g_beacon, STATION_ID,
                   GPIO_IR_A, GPIO_IR_B, GPIO_IR_C);
    charge_ctl_init(&g_charge,
                    GPIO_PWR_EN, GPIO_VSENSE, GPIO_ISENSE);
    status_indicator_init(&g_indicator,
                          GPIO_RGB_R, GPIO_RGB_G, GPIO_RGB_B);

    // ── 3. Start IR beacon ──
    ir_beacon_start(&g_beacon);

    // ── 4. Init WiFi reporter ──
    wifi_reporter_init(&g_reporter,
                       WIFI_SSID, WIFI_PASS,
                       GATEWAY_HOST, GATEWAY_PORT,
                       STATION_ID);
    g_report_queue = wifi_reporter_start(&g_reporter);

    // ── 5. Start LED blink task ──
    xTaskCreate(led_blink_task, "led_blink", 2048, NULL, 3, NULL);

    // Update status after WiFi connects
    status_indicator_set(&g_indicator, STATUS_CONNECTING);

    ESP_LOGI(TAG, "startup complete, entering main loop");

    // ── 6. Main control loop (100ms tick) ──
    uint32_t loop_count = 0;
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(100));

        // Charge controller FSM
        charge_ctl_tick(&g_charge);

        // Update status LED based on charge state
        charge_state_t cs = charge_ctl_get_state(&g_charge);
        switch (cs) {
        case CHARGE_IDLE:
            if (g_reporter.wifi_state == WIFI_CONNECTED) {
                status_indicator_set(&g_indicator, STATUS_IDLE);
            } else {
                status_indicator_set(&g_indicator, STATUS_CONNECTING);
            }
            break;
        case CHARGE_DOCKED:
        case CHARGE_ACTIVE:
            status_indicator_set(&g_indicator, STATUS_CHARGING);
            break;
        case CHARGE_FAULT:
            status_indicator_set(&g_indicator, STATUS_FAULT);
            break;
        // CHARGE_COMPLETE handled by blink task
        default:
            break;
        }

        // Periodic status report to gateway (every 5s)
        if (++loop_count % 50 == 0 && g_report_queue) {
            reporter_event_t ev = {
                .station_id   = STATION_ID,
                .charge_state = (uint8_t)cs,
                .v_out        = charge_ctl_get_voltage(&g_charge),
                .i_out        = charge_ctl_get_current(&g_charge),
                .uptime_s     = (uint32_t)(esp_timer_get_time() / 1000000),
            };
            wifi_reporter_report(g_report_queue, &ev);
        }

        // Auto-disable beacon while robot is docked (save power)
        if (cs == CHARGE_ACTIVE || cs == CHARGE_COMPLETE) {
            ir_beacon_stop(&g_beacon);
        } else {
            ir_beacon_start(&g_beacon);
        }
    }
}
