/**
 * Charging station interface.
 *
 * Detects docked state via GPIO (IR proximity sensor).
 * Communicates with charging station MCU over one-wire UART (9600 baud).
 * Handshake: send 0x55, expect 0xAA response within 100ms.
 */

#include "charging.h"
#include "config.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"

static const char *TAG = "charging";

/* ── Pin definitions ──────────────────────────────────────────── */

#define CHARGE_DETECT_IO    17    /* IR proximity sensor (GPIO interrupt) */
#define CHARGE_UART_TX_IO   16    /* one-wire UART TX */
#define CHARGE_UART_RX_IO   16    /* one-wire UART RX (same pin for one-wire) */
#define CHARGE_UART_NUM     UART_NUM_1
#define CHARGE_UART_BAUD    9600

/* ── Handshake ────────────────────────────────────────────────── */

#define HANDSHAKE_CHALLENGE 0x55
#define HANDSHAKE_RESPONSE  0xAA
#define HANDSHAKE_TIMEOUT_MS 100

/* ── State ────────────────────────────────────────────────────── */

static charge_state_t g_charge_state = CHARGE_NOT_CONNECTED;
static bool g_uart_ok;

/* ── GPIO ISR ─────────────────────────────────────────────────── */

static void IRAM_ATTR charge_detect_isr(void *arg) {
    /* Wakes up the power manager task to check charging state.
     * ISR is kept minimal — only queues to a FreeRTOS task. */
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    /* We use a simple flag; in production use a task notification or semaphore */
    /* xTaskNotifyFromISR(power_mgr_task_handle, 0, eNoAction, &xHigherPriorityTaskWoken); */

    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

/* ── One-wire handshake ───────────────────────────────────────── */

static bool charging_handshake(void) {
    if (!g_uart_ok) return false;

    /* Send challenge byte */
    uint8_t challenge = HANDSHAKE_CHALLENGE;
    uart_write_bytes(CHARGE_UART_NUM, &challenge, 1);

    /* Wait for response */
    uint8_t response = 0;
    int bytes_read = uart_read_bytes(CHARGE_UART_NUM, &response, 1,
                                     pdMS_TO_TICKS(HANDSHAKE_TIMEOUT_MS));
    if (bytes_read == 1 && response == HANDSHAKE_RESPONSE) {
        ESP_LOGI(TAG, "charging station handshake OK");
        return true;
    }

    ESP_LOGW(TAG, "charging station handshake failed (resp=0x%02X)", response);
    return false;
}

/* ── Public API ───────────────────────────────────────────────── */

void charging_init(void) {
    ESP_LOGI(TAG, "initializing charging station interface");

    /* Configure dock detect GPIO with interrupt */
    gpio_config_t detect_cfg = {
        .pin_bit_mask = (1ULL << CHARGE_DETECT_IO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,  /* both dock and undock */
    };
    ESP_ERROR_CHECK(gpio_config(&detect_cfg));

    /* Install ISR */
    gpio_install_isr_service(0);
    gpio_isr_handler_add(CHARGE_DETECT_IO, charge_detect_isr, NULL);

    /* Configure one-wire UART */
    uart_config_t uart_cfg = {
        .baud_rate  = CHARGE_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(CHARGE_UART_NUM, 256, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "UART driver install failed: %d", err);
        g_uart_ok = false;
        return;
    }

    err = uart_param_config(CHARGE_UART_NUM, &uart_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "UART param config failed: %d", err);
        g_uart_ok = false;
        return;
    }

    err = uart_set_pin(CHARGE_UART_NUM, CHARGE_UART_TX_IO, CHARGE_UART_RX_IO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "UART set pin failed: %d", err);
        g_uart_ok = false;
        return;
    }

    g_uart_ok = true;

    /* Check initial dock state */
    int level = gpio_get_level(CHARGE_DETECT_IO);
    if (level == 0) {
        /* Pull-up active → LOW = docked (IR beam broken) */
        ESP_LOGI(TAG, "robot is docked at startup");
        g_charge_state = CHARGE_DOCKED_IDLE;

        /* Try handshake */
        if (charging_handshake()) {
            g_charge_state = CHARGE_CC;  /* start constant-current charging */
        }
    } else {
        g_charge_state = CHARGE_NOT_CONNECTED;
    }

    ESP_LOGI(TAG, "charging interface ready (state=%d)", g_charge_state);
}

charge_state_t charging_get_state(void) {
    /* Update from GPIO */
    int level = gpio_get_level(CHARGE_DETECT_IO);
    bool docked = (level == 0);  /* active low */

    if (!docked) {
        g_charge_state = CHARGE_NOT_CONNECTED;
    } else if (g_charge_state == CHARGE_NOT_CONNECTED) {
        /* Just docked — start charging */
        g_charge_state = CHARGE_DOCKED_IDLE;
        if (charging_handshake()) {
            g_charge_state = CHARGE_CC;
        }
    }

    return g_charge_state;
}

bool charging_is_docked(void) {
    return charging_get_state() != CHARGE_NOT_CONNECTED;
}
