/**
 * Companion Robot — ESP32-S3 Firmware Entry Point.
 *
 * Hardware initialization, FreeRTOS task creation, system startup.
 * Core 0: communication tasks (WiFi/BLE, BCP codec, connection manager)
 * Core 1: perception + execution tasks (sensors, motors, behavior)
 */

#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_bt.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"

/* BCP protocol */
#include "bcp_codec.h"

/* Component headers */
#include "conn_manager.h"
#include "ws_client.h"
#include "ble_gatt.h"
#include "motor.h"
#include "servo.h"
#include "camera.h"
#include "microphone.h"
#include "speaker.h"
#include "imu.h"
#include "env_sensor.h"
#include "tof.h"
#include "touch.h"
#include "led.h"
#include "face.h"
#include "power_mgr.h"
#include "battery.h"
#include "charging.h"
#include "localization.h"
#include "log_upload.h"
#include "behavior.h"
#include "offline.h"
#include "ota.h"
#include "nvs_config.h"

static const char *TAG = "main";

/* ── Global queues and event groups ───────────────────────── */

QueueHandle_t q_cmd_incoming;
QueueHandle_t q_cmd_outgoing;
QueueHandle_t q_sensor_event;
QueueHandle_t q_audio_frame;
QueueHandle_t q_vision_frame;
EventGroupHandle_t evg_system;

/* Event bits */
#define EVG_WIFI_CONNECTED     BIT0
#define EVG_WS_CONNECTED       BIT1
#define EVG_BLE_CONNECTED      BIT2
#define EVG_SENSOR_READY       BIT3
#define EVG_EMERGENCY          BIT4
#define EVG_LOW_BATTERY        BIT5

/* ── Hardware initialization ──────────────────────────────── */

static void init_nvs(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

static void init_i2c(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0));
}

static void init_spi(void) {
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CAM_MOSI_IO,
        .miso_io_num = CAM_MISO_IO,
        .sclk_io_num = CAM_SCK_IO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32768,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));
}

/* ── Watchdog registration ────────────────────────────────── */

static void register_watchdog(TaskHandle_t task) {
    esp_task_wdt_add(task);
}

/* ── Task creation ────────────────────────────────────────── */

void app_main(void) {
    ESP_LOGI(TAG, "=== Companion Robot Firmware v0.1.0 ===");
    ESP_LOGI(TAG, "Target: ESP32-S3, dual-core 240MHz");

    /* ── Hardware init ── */
    init_nvs();
    init_i2c();
    init_spi();

    /* Initialize component drivers */
    imu_init();
    env_sensor_init();
    tof_init();
    touch_init();
    led_init();
    face_init();
    speaker_init();
    microphone_init();
    camera_init();
    localization_init();
    log_upload_init();
    motor_init();
    servo_init();
    battery_init();
    charging_init();
    power_mgr_init();

    ESP_LOGI(TAG, "hardware initialization complete");

    /* ── Create queues ── */
    q_cmd_incoming = xQueueCreate(Q_CMD_INCOMING_DEPTH, sizeof(bcp_command_t));
    q_cmd_outgoing = xQueueCreate(Q_CMD_OUTGOING_DEPTH, sizeof(bcp_frame_t));
    q_sensor_event = xQueueCreate(Q_SENSOR_EVENT_DEPTH, sizeof(uint32_t));
    q_audio_frame  = xQueueCreate(Q_AUDIO_FRAME_DEPTH, sizeof(uint32_t));
    q_vision_frame = xQueueCreate(Q_VISION_FRAME_DEPTH, sizeof(uint32_t));
    evg_system     = xEventGroupCreate();

    ESP_LOGI(TAG, "queues and event groups created");

    /* ── Init task watchdog ── */
    esp_task_wdt_init(WATCHDOG_TIMEOUT_MS / 1000, true);

    /* ── Core 0: Communication tasks ── */
    TaskHandle_t h_comm_rx, h_comm_tx, h_conn_mgr;
    xTaskCreatePinnedToCore(task_comm_rx, "comm_rx",
        STACK_COMM_RX, NULL, PRIO_COMM_RX, &h_comm_rx, 0);
    register_watchdog(h_comm_rx);

    xTaskCreatePinnedToCore(task_comm_tx, "comm_tx",
        STACK_COMM_TX, NULL, PRIO_COMM_TX, &h_comm_tx, 0);
    register_watchdog(h_comm_tx);

    xTaskCreatePinnedToCore(task_conn_manager, "conn_mgr",
        STACK_CONN_MGR, NULL, PRIO_CONN_MGR, &h_conn_mgr, 0);
    register_watchdog(h_conn_mgr);

    ESP_LOGI(TAG, "Core 0 tasks created (comm_rx, comm_tx, conn_mgr)");

    /* ── Core 1: Perception + Execution tasks ── */
    TaskHandle_t h_vision, h_audio_in, h_audio_out, h_imu;
    TaskHandle_t h_motor, h_servo, h_env, h_touch, h_led, h_localization, h_behavior, h_power;

    xTaskCreatePinnedToCore(task_vision, "vision",
        STACK_VISION, NULL, PRIO_VISION, &h_vision, 1);
    register_watchdog(h_vision);

    xTaskCreatePinnedToCore(task_audio_in, "audio_in",
        STACK_AUDIO_IN, NULL, PRIO_AUDIO_IN, &h_audio_in, 1);
    register_watchdog(h_audio_in);

    xTaskCreatePinnedToCore(task_audio_out, "audio_out",
        STACK_AUDIO_OUT, NULL, PRIO_AUDIO_OUT, &h_audio_out, 1);

    xTaskCreatePinnedToCore(task_imu, "imu",
        STACK_IMU, NULL, PRIO_IMU, &h_imu, 1);
    register_watchdog(h_imu);

    xTaskCreatePinnedToCore(task_motor_ctrl, "motor",
        STACK_MOTOR, NULL, PRIO_MOTOR, &h_motor, 1);
    register_watchdog(h_motor);

    xTaskCreatePinnedToCore(task_servo_ctrl, "servo",
        STACK_SERVO, NULL, PRIO_SERVO, &h_servo, 1);

    xTaskCreatePinnedToCore(task_env_sensor, "env",
        STACK_ENV_SENSOR, NULL, PRIO_ENV_SENSOR, &h_env, 1);

    xTaskCreatePinnedToCore(task_touch, "touch",
        STACK_TOUCH, NULL, PRIO_TOUCH, &h_touch, 1);

    xTaskCreatePinnedToCore(task_led, "led",
        STACK_LED, NULL, PRIO_LED, &h_led, 1);

    xTaskCreatePinnedToCore(task_localization, "localization",
        STACK_LOCALIZATION, NULL, PRIO_LOCALIZATION, &h_localization, 1);
    register_watchdog(h_localization);

    xTaskCreatePinnedToCore(task_behavior, "behavior",
        STACK_BEHAVIOR, NULL, PRIO_BEHAVIOR, &h_behavior, 1);

    xTaskCreatePinnedToCore(task_power_mgr, "power",
        STACK_POWER, NULL, PRIO_POWER, &h_power, 1);

    ESP_LOGI(TAG, "Core 1 tasks created (13 tasks)");
    ESP_LOGI(TAG, "system ready — entering scheduler");

    /* FreeRTOS scheduler takes over */
}
