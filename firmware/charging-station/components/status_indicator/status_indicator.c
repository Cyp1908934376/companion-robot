#include "status_indicator.h"
#include "esp_log.h"

#define TAG "status_indicator"

#define LEDC_FREQ_HZ  5000
#define LEDC_RES      LEDC_TIMER_8_BIT

void status_indicator_init(status_indicator_t *ind,
                           uint8_t gpio_r, uint8_t gpio_g, uint8_t gpio_b)
{
    ind->gpio_r = gpio_r;
    ind->gpio_g = gpio_g;
    ind->gpio_b = gpio_b;
    ind->state  = STATUS_CONNECTING;

    ledc_timer_config_t timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .duty_resolution  = LEDC_RES,
        .timer_num        = LEDC_TIMER_0,
        .freq_hz          = LEDC_FREQ_HZ,
        .clk_cfg          = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t ch_r = {
        .gpio_num   = gpio_r,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config_t ch_g = ch_r;
    ch_g.gpio_num = gpio_g;
    ch_g.channel  = LEDC_CHANNEL_1;
    ledc_channel_config_t ch_b = ch_r;
    ch_b.gpio_num = gpio_b;
    ch_b.channel  = LEDC_CHANNEL_2;

    ESP_ERROR_CHECK(ledc_channel_config(&ch_r));
    ESP_ERROR_CHECK(ledc_channel_config(&ch_g));
    ESP_ERROR_CHECK(ledc_channel_config(&ch_b));

    ind->ch_r = ch_r;
    ind->ch_g = ch_g;
    ind->ch_b = ch_b;

    status_indicator_set(ind, STATUS_CONNECTING);
    ESP_LOGI(TAG, "initialized r=%d g=%d b=%d", gpio_r, gpio_g, gpio_b);
}

void status_indicator_set_rgb(status_indicator_t *ind,
                              uint8_t r, uint8_t g, uint8_t b)
{
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, r));
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, g));
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, b));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
}

void status_indicator_set(status_indicator_t *ind, indicator_state_t state)
{
    ind->state = state;
    switch (state) {
    case STATUS_IDLE:
        status_indicator_set_rgb(ind, 0, 128, 0);    // green solid
        break;
    case STATUS_CHARGING:
        status_indicator_set_rgb(ind, 0, 64, 255);   // blue
        break;
    case STATUS_COMPLETE:
        status_indicator_set_rgb(ind, 0, 255, 0);    // green (blink handled by main loop)
        break;
    case STATUS_FAULT:
        status_indicator_set_rgb(ind, 255, 30, 0);   // red
        break;
    case STATUS_CONNECTING:
        status_indicator_set_rgb(ind, 255, 200, 0);  // yellow
        break;
    }
}
