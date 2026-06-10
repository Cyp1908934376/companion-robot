#include "charge_ctl.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "esp_log.h"
#include "esp_timer.h"

#define TAG "charge_ctl"

// ── ADC calibration ─────────────────────────────────────────────

static esp_adc_cal_characteristics_t adc1_chars;

void charge_ctl_init(charge_ctl_t *ctl, uint8_t pwr_en, uint8_t vsense, uint8_t isense)
{
    ctl->pwr_en_gpio   = pwr_en;
    ctl->adc_vsense_gpio = vsense;
    ctl->adc_isense_gpio = isense;
    ctl->v_nominal       = 5.0f;
    ctl->i_max           = 2.0f;
    ctl->i_threshold_full = 0.1f;
    ctl->precharge_ms    = 500;
    ctl->state           = CHARGE_IDLE;
    ctl->docked          = false;
    ctl->v_out           = 0.0f;
    ctl->i_out           = 0.0f;
    ctl->docked_at_ms    = 0;
    ctl->fault_count     = 0;

    // Power enable GPIO
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << pwr_en),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_cfg);
    gpio_set_level(pwr_en, 0);  // start with output off

    // ADC1 init
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_11);  // 0~3.6V range
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12,
                             1100, &adc1_chars);

    ESP_LOGI(TAG, "initialized pwr_en=GPIO%d vsense=GPIO%d",
             pwr_en, vsense);
}

void charge_ctl_set_output(charge_ctl_t *ctl, bool on)
{
    gpio_set_level(ctl->pwr_en_gpio, on ? 1 : 0);
}

static float read_voltage_adc(void)
{
    uint32_t raw = adc1_get_raw(ADC1_CHANNEL_0);
    uint32_t mv  = esp_adc_cal_raw_to_voltage(raw, &adc1_chars);
    // Voltage divider 2:1 → actual voltage = 2 × measured
    return (float)mv * 2.0f / 1000.0f;
}

static float read_current_adc(void)
{
    // ACS712 or shunt + amp; 185mV/A typical
    uint32_t raw = adc1_get_raw(ADC1_CHANNEL_0);
    uint32_t mv  = esp_adc_cal_raw_to_voltage(raw, &adc1_chars);
    return (float)mv / 185.0f;
}

void charge_ctl_tick(charge_ctl_t *ctl)
{
    ctl->v_out = read_voltage_adc();
    ctl->i_out = read_current_adc();

    uint32_t now_ms = esp_timer_get_time() / 1000;

    switch (ctl->state) {
    case CHARGE_IDLE:
        // Detect docking: voltage rises when contacts touch robot
        if (ctl->v_out > 1.0f && !ctl->docked) {
            ctl->docked       = true;
            ctl->docked_at_ms = now_ms;
            ctl->state        = CHARGE_DOCKED;
            ESP_LOGI(TAG, "robot docked, v=%.2fV", ctl->v_out);
        }
        break;

    case CHARGE_DOCKED:
        if (now_ms - ctl->docked_at_ms > ctl->precharge_ms) {
            charge_ctl_set_output(ctl, true);
            ctl->state = CHARGE_ACTIVE;
            ESP_LOGI(TAG, "charge enabled");
        }
        break;

    case CHARGE_ACTIVE:
        // Overcurrent protection
        if (ctl->i_out > ctl->i_max) {
            charge_ctl_set_output(ctl, false);
            ctl->state = CHARGE_FAULT;
            ctl->fault_count++;
            ESP_LOGW(TAG, "overcurrent fault i=%.2fA", ctl->i_out);
            break;
        }
        // Full detection: current drops below C/20
        if (ctl->i_out < ctl->i_threshold_full) {
            charge_ctl_set_output(ctl, false);
            ctl->state = CHARGE_COMPLETE;
            ESP_LOGI(TAG, "charge complete");
            break;
        }
        // Robot undocked → contact open
        if (ctl->v_out < 0.5f) {
            charge_ctl_set_output(ctl, false);
            ctl->docked = false;
            ctl->state  = CHARGE_IDLE;
            ESP_LOGI(TAG, "robot undocked");
        }
        break;

    case CHARGE_COMPLETE:
        // Wait for undock
        if (ctl->v_out < 0.5f) {
            charge_ctl_set_output(ctl, false);
            ctl->docked = false;
            ctl->state  = CHARGE_IDLE;
        }
        break;

    case CHARGE_FAULT:
        // Latch fault; clear on undock + timeout
        if (ctl->v_out < 0.5f) {
            ctl->docked = false;
            ctl->state  = CHARGE_IDLE;
            ESP_LOGI(TAG, "fault cleared by undock");
        }
        break;
    }
}

charge_state_t charge_ctl_get_state(const charge_ctl_t *ctl) { return ctl->state; }
float charge_ctl_get_voltage(const charge_ctl_t *ctl) { return ctl->v_out; }
float charge_ctl_get_current(const charge_ctl_t *ctl) { return ctl->i_out; }
