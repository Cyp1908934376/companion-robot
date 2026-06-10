/// RGB LED status indicator for the charging station.
///
/// Color codes:
///   Green solid    — Idle, ready for docking
///   Blue pulse     — Robot docked, charging
///   Green blink    — Charge complete, robot still docked
///   Red blink      — Fault (overcurrent / short)
///   Yellow solid   — Initializing / WiFi connecting

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/ledc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STATUS_IDLE,
    STATUS_CHARGING,
    STATUS_COMPLETE,
    STATUS_FAULT,
    STATUS_CONNECTING,
} indicator_state_t;

typedef struct {
    uint8_t gpio_r;
    uint8_t gpio_g;
    uint8_t gpio_b;
    ledc_channel_config_t ch_r;
    ledc_channel_config_t ch_g;
    ledc_channel_config_t ch_b;
    indicator_state_t    state;
} status_indicator_t;

/// Initialize 3 PWM channels for RGB LED.
void status_indicator_init(status_indicator_t *ind,
                           uint8_t gpio_r, uint8_t gpio_g, uint8_t gpio_b);

/// Set the indicator to a predefined state/color.
void status_indicator_set(status_indicator_t *ind, indicator_state_t state);

/// Raw RGB set (0-255 per channel).
void status_indicator_set_rgb(status_indicator_t *ind,
                              uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif
