/**
 * Power manager — coordinates battery, charging, power states, and sleep modes.
 *
 * Power states:
 *   POWER_NORMAL       → full operation
 *   POWER_LOW_BATTERY  → disable camera, reduce motor speed
 *   POWER_CRITICAL     → motors off, only comm + LEDs
 *   POWER_CHARGING     → sensors active, motors disabled
 *   POWER_CHARGED      → trickle charge, systems idle
 *   POWER_FAULT        → over-temp or charge timeout
 *
 * Escalating idle → sleep:
 *   5min idle  → disable camera, CPU 80MHz
 *   30min idle → Light Sleep (WiFi DTIM wake, 0.8mA)
 *   60min idle → Deep Sleep if battery < 20% (RTC wake, 10µA)
 *
 * 2Hz polling. Sets event group bits for cross-task notification.
 */

#include "power_mgr.h"
#include "battery.h"
#include "charging.h"
#include "config.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_task_wdt.h"

static const char *TAG = "power";

/* ── State ────────────────────────────────────────────────────── */

static power_state_t g_power_state = POWER_NORMAL;
static power_state_t g_prev_state = POWER_NORMAL;

/* Idle/escalation tracking */
typedef enum {
    SLEEP_LEVEL_ACTIVE = 0,
    SLEEP_LEVEL_IDLE,        /* camera off, CPU 80MHz */
    SLEEP_LEVEL_LIGHT,       /* entering light sleep */
    SLEEP_LEVEL_DEEP,        /* entering deep sleep */
} sleep_level_t;

static sleep_level_t g_sleep_level = SLEEP_LEVEL_ACTIVE;
static uint32_t g_last_activity_ms;
static uint32_t g_entered_idle_ms;

extern EventGroupHandle_t evg_system;
extern QueueHandle_t q_cmd_incoming;

/* ── RTC memory (survives deep sleep) ─────────────────────────── */

#define RTC_DATA_MAGIC 0xB007  /* "BOOT" */

typedef struct {
    uint16_t magic;
    uint8_t  boot_count;
    uint8_t  last_soc;
    uint32_t total_sleep_ms;
} rtc_data_t;

static RTC_DATA_ATTR rtc_data_t g_rtc_data;

/* ── Wake source configuration ────────────────────────────────── */

static void configure_wake_sources(void) {
    /* GPIO wake: touch zones (RTC GPIO) and charge detect */
    const int wake_pins[] = {
        TOUCH_ZONE_HEAD,
        TOUCH_ZONE_BACK,
        TOUCH_ZONE_LEFT,
        TOUCH_ZONE_RIGHT,
        17,  /* charge detect */
    };

    for (int i = 0; i < sizeof(wake_pins) / sizeof(wake_pins[0]); i++) {
        gpio_wakeup_enable(wake_pins[i], GPIO_INTR_LOW_LEVEL);
    }

    /* Touch pad wake */
    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
    touch_pad_fsm_start();

    /* ULP co-processor: sample battery voltage during deep sleep (optional) */
    /* ulp_program_load_and_start(); — Phase 3+ */
}

/* ── Sleep entry ──────────────────────────────────────────────── */

static uint32_t g_total_sleep_ms;

static void enter_light_sleep(void) {
    g_sleep_level = SLEEP_LEVEL_LIGHT;

    /* Disable unneeded peripherals before sleep */
    /* Camera, motors, ToF, mic, speaker — powered down by their tasks */

    ESP_LOGI(TAG, "entering light sleep (wake every %dms)", LIGHT_SLEEP_WAKE_MS);

    /* Configure timer wake */
    esp_sleep_enable_timer_wakeup(LIGHT_SLEEP_WAKE_MS * 1000ULL);

    /* GPIO wake for touch/charge events */
    esp_sleep_enable_gpio_wakeup();

    /* WiFi DTIM wake: ESP32 wakes on WiFi beacon (keeps connection alive) */
    esp_sleep_enable_wifi_wakeup();

    /* Enter light sleep */
    vTaskDelay(pdMS_TO_TICKS(10));  /* let serial flush */
    esp_err_t err = esp_light_sleep_start();

    if (err == ESP_OK) {
        uint32_t slept_ms = LIGHT_SLEEP_WAKE_MS;  /* approximate */
        g_total_sleep_ms += slept_ms;
        ESP_LOGD(TAG, "light sleep wake (total sleep: %lu ms)", g_total_sleep_ms);
    } else {
        ESP_LOGW(TAG, "light sleep failed: %d", err);
    }

    /* Wake: check state */
    g_sleep_level = SLEEP_LEVEL_ACTIVE;
    g_last_activity_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static void enter_deep_sleep(void) {
    battery_status_t bat = battery_get_status();

    ESP_LOGW(TAG, "entering deep sleep (SOC=%d%%, wake every %ds)",
             bat.soc_pct, DEEP_SLEEP_WAKE_SEC);

    /* Save state to RTC memory */
    g_rtc_data.magic          = RTC_DATA_MAGIC;
    g_rtc_data.boot_count     = (g_rtc_data.magic == RTC_DATA_MAGIC)
                                ? g_rtc_data.boot_count + 1 : 1;
    g_rtc_data.last_soc       = bat.soc_pct;
    g_rtc_data.total_sleep_ms = g_rtc_data.total_sleep_ms + g_total_sleep_ms;

    /* Configure deep sleep wake sources */
    esp_sleep_enable_timer_wakeup(DEEP_SLEEP_WAKE_SEC * 1000000ULL);

    /* Touch wake (any touch pad) */
    touch_pad_t touch_pins[] = { TOUCH_ZONE_HEAD, TOUCH_ZONE_BACK,
                                 TOUCH_ZONE_LEFT, TOUCH_ZONE_RIGHT };
    for (int i = 0; i < 4; i++) {
        esp_sleep_enable_touchpad_wakeup();
    }

    /* Charge detect GPIO wake */
    esp_sleep_enable_ext0_wakeup(17, 0);  /* charge detect LOW = docked */

    /* Disable WiFi and BT to save power before deep sleep */
    /* These are handled by conn_manager based on EVG bits */

    /* Power down peripherals */
    /* I2C, SPI, I2S — powered down by sleep controller */

    ESP_LOGI(TAG, "deep sleep entry (boot #%d)", g_rtc_data.boot_count);

    vTaskDelay(pdMS_TO_TICKS(50));  /* flush logs */
    esp_deep_sleep_start();

    /* Execution resumes here after wake (CPU restarts from bootloader) */
}

/* ── Activity tracking ────────────────────────────────────────── */

static void report_activity(void) {
    g_last_activity_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (g_sleep_level != SLEEP_LEVEL_ACTIVE) {
        ESP_LOGI(TAG, "activity detected — exiting sleep level %d", g_sleep_level);
        g_sleep_level = SLEEP_LEVEL_ACTIVE;
    }
}

static bool is_idle(uint32_t timeout_ms) {
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    return (now - g_last_activity_ms) > timeout_ms;
}

/* ── Public API ───────────────────────────────────────────────── */

void power_mgr_init(void) {
    ESP_LOGI(TAG, "initializing power manager");

    /* Check for wake from deep sleep */
    if (g_rtc_data.magic == RTC_DATA_MAGIC) {
        ESP_LOGI(TAG, "woke from deep sleep (boot #%d, total_sleep=%lu ms)",
                 g_rtc_data.boot_count, g_rtc_data.total_sleep_ms);
        g_total_sleep_ms = g_rtc_data.total_sleep_ms;
    } else {
        g_rtc_data = (rtc_data_t){0};
    }

    /* Configure wake sources */
    configure_wake_sources();

    /* Read initial battery and charging states */
    battery_status_t bat = battery_get_status();
    charge_state_t charge = charging_get_state();

    ESP_LOGI(TAG, "initial: SOC=%d%% V=%.2fV charge=%d sleep_total=%lu ms",
             bat.soc_pct, bat.voltage, charge, g_total_sleep_ms);

    /* Determine initial power state */
    if (bat.critical) {
        g_power_state = POWER_CRITICAL;
    } else if (bat.low) {
        g_power_state = POWER_LOW_BATTERY;
    } else if (charging_is_docked()) {
        g_power_state = (bat.soc_pct >= BATTERY_EXIT_CHARGE_PCT)
                        ? POWER_CHARGED : POWER_CHARGING;
    } else {
        g_power_state = POWER_NORMAL;
    }

    g_prev_state  = g_power_state;
    g_sleep_level = SLEEP_LEVEL_ACTIVE;
    g_last_activity_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    ESP_LOGI(TAG, "power state: %d sleep_level: %d", g_power_state, g_sleep_level);
}

power_state_t power_mgr_get_state(void) {
    return g_power_state;
}

void power_mgr_report_activity(void) {
    report_activity();
}

/* ── Power management task ────────────────────────────────────── */

void task_power_mgr(void *arg) {
    ESP_LOGI(TAG, "power management task started (2Hz)");

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(500);  /* 2Hz */

    while (1) {
        /* Read sensors */
        battery_status_t bat = battery_get_status();
        charge_state_t charge = charging_get_state();
        bool docked = charging_is_docked();

        /* ── Activity detection ────────────────────────────────── */
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        /* Check if motor command queue has pending commands */
        uint32_t pending_msgs = uxQueueMessagesWaiting(q_cmd_incoming);
        if (pending_msgs > 0) {
            report_activity();
        }

        /* ── Idle → sleep escalation ──────────────────────────── */
        if (!docked && g_power_state == POWER_NORMAL) {
            if (is_idle(DEEP_SLEEP_TIMEOUT_MS) && bat.soc_pct < BATTERY_LOW_PCT) {
                /* Deep sleep: long idle + low battery */
                if (g_sleep_level != SLEEP_LEVEL_DEEP) {
                    ESP_LOGW(TAG, "escalating to DEEP SLEEP (SOC=%d%%)", bat.soc_pct);
                    enter_deep_sleep();
                    /* After waking, re-init peripherals */
                    g_last_activity_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
                }
            } else if (is_idle(LIGHT_SLEEP_TIMEOUT_MS)) {
                /* Light sleep: extended idle */
                if (g_sleep_level != SLEEP_LEVEL_LIGHT) {
                    ESP_LOGI(TAG, "escalating to LIGHT SLEEP (%lu min idle)",
                             (now - g_last_activity_ms) / 60000);
                    enter_light_sleep();
                }
            } else if (is_idle(IDLE_TIMEOUT_MS) && g_sleep_level == SLEEP_LEVEL_ACTIVE) {
                /* Enter IDLE: disable camera, lower CPU freq */
                g_sleep_level = SLEEP_LEVEL_IDLE;
                g_entered_idle_ms = now;
                ESP_LOGI(TAG, "entering IDLE mode (disable camera, CPU 80MHz)");
                /* In production: esp_pm_lock_acquire(ESP_PM_CPU_FREQ_MAX, ...) */
            }
        } else if (docked || g_power_state != POWER_NORMAL) {
            /* Don't sleep while docked or in non-normal states */
            report_activity();
        }

        /* ── State transitions ────────────────────────────────── */
        power_state_t new_state = g_power_state;

        if (g_power_state == POWER_FAULT) {
            if (!bat.critical && !docked) {
                new_state = POWER_NORMAL;
            }
        } else if (bat.critical && !docked) {
            new_state = POWER_CRITICAL;
        } else if (bat.low && !bat.critical && !docked) {
            new_state = POWER_LOW_BATTERY;
        } else if (docked) {
            if (bat.soc_pct >= BATTERY_EXIT_CHARGE_PCT) {
                new_state = POWER_CHARGED;
            } else {
                new_state = POWER_CHARGING;
            }
        } else {
            new_state = POWER_NORMAL;
        }

        /* ── Apply state changes ──────────────────────────────── */
        if (new_state != g_power_state) {
            ESP_LOGI(TAG, "power state transition: %d → %d (SOC=%d%% docked=%d sleep=%d)",
                     g_power_state, new_state, bat.soc_pct, docked, g_sleep_level);
            g_prev_state = g_power_state;
            g_power_state = new_state;
            report_activity();  /* state change is activity */
        }

        /* ── Update event group bits ──────────────────────────── */
        switch (g_power_state) {
        case POWER_NORMAL:
            xEventGroupClearBits(evg_system, EVG_LOW_BATTERY | EVG_EMERGENCY);
            break;
        case POWER_LOW_BATTERY:
            xEventGroupSetBits(evg_system, EVG_LOW_BATTERY);
            xEventGroupClearBits(evg_system, EVG_EMERGENCY);
            break;
        case POWER_CRITICAL:
            xEventGroupSetBits(evg_system, EVG_LOW_BATTERY | EVG_EMERGENCY);
            break;
        case POWER_CHARGING:
            xEventGroupClearBits(evg_system, EVG_LOW_BATTERY | EVG_EMERGENCY);
            break;
        case POWER_CHARGED:
            xEventGroupClearBits(evg_system, EVG_LOW_BATTERY | EVG_EMERGENCY);
            break;
        case POWER_FAULT:
            xEventGroupSetBits(evg_system, EVG_EMERGENCY);
            break;
        }

        /* ── Log periodic status ──────────────────────────────── */
        static int log_counter = 0;
        if (++log_counter % 10 == 0) {  /* every 5s at 2Hz */
            ESP_LOGI(TAG, "state=%d sleep=%d SOC=%d%% V=%.2fV I=%.0fmA docked=%d",
                     g_power_state, g_sleep_level, bat.soc_pct,
                     bat.voltage, bat.current_ma, docked);
        }

        vTaskDelayUntil(&last_wake, period);
        esp_task_wdt_reset();
    }
}
