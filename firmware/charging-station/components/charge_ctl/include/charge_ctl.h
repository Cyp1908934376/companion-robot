/// Charge controller — manages power delivery and docking detection.
///
/// Hardware:
///   - Pogo pins (2): VCC + GND, spring-loaded contacts mate with robot belly
///   - INA226: I2C current/voltage monitor (optional, GPIO ADC fallback)
///   - MOSFET: high-side switch to enable/disable charge output
///   - Dock detect: contact closure → ADC voltage change
///
/// Safety:
///   - Overcurrent cutoff at 2.5A
///   - Short-circuit detection (< 200ms response)
///   - Battery full detection (current drops below C/20 threshold)
///   - Reverse polarity protection (hardware diode)

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CHARGE_IDLE        = 0,  // no robot docked
    CHARGE_DOCKED      = 1,  // robot detected, waiting to enable
    CHARGE_ACTIVE      = 2,  // charging in progress
    CHARGE_COMPLETE    = 3,  // robot battery full
    CHARGE_FAULT       = 4,  // overcurrent / short / temp
} charge_state_t;

typedef struct {
    // GPIO config
    uint8_t     pwr_en_gpio;      // MOSFET gate control
    uint8_t     adc_vsense_gpio;  // voltage sense ADC
    uint8_t     adc_isense_gpio;  // current sense ADC (optional)
    // Limits
    float       v_nominal;        // nominal output voltage (5.0V)
    float       i_max;            // max charge current (2.0A)
    float       i_threshold_full; // C/20 threshold for full detection
    uint32_t    precharge_ms;     // delay before enabling power after dock
    // State
    charge_state_t state;
    bool        docked;
    float       v_out;
    float       i_out;
    uint32_t    docked_at_ms;
    uint32_t    fault_count;
} charge_ctl_t;

/// Initialize GPIOs, ADC, and default state.
void charge_ctl_init(charge_ctl_t *ctl, uint8_t pwr_en, uint8_t vsense, uint8_t isense);

/// Called periodically (100ms loop). Reads ADC, updates state machine.
void charge_ctl_tick(charge_ctl_t *ctl);

/// Enable or disable the charge output MOSFET.
void charge_ctl_set_output(charge_ctl_t *ctl, bool on);

/// Returns current state for status LED / WiFi reporting.
charge_state_t charge_ctl_get_state(const charge_ctl_t *ctl);

/// Returns measured output voltage.
float charge_ctl_get_voltage(const charge_ctl_t *ctl);

/// Returns measured output current.
float charge_ctl_get_current(const charge_ctl_t *ctl);

#ifdef __cplusplus
}
#endif
