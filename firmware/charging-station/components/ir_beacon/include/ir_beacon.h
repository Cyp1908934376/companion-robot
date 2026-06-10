/// IR beacon driver for robot docking guidance.
///
/// Generates 38kHz carrier with PPM (pulse-position modulation)
/// to encode the charging station ID. Three IR LEDs emit phase-shifted
/// signals so the robot's 3 receivers can triangulate approach direction.
///
/// Signal format: [Preamble 4ms] [Station ID 8bit] [Checksum 4bit]
/// Repeat interval: 100ms

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/rmt_tx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IR_CARRIER_HZ       38000
#define IR_PREAMBLE_US      4000
#define IR_REPEAT_INTERVAL_MS 100
#define IR_BEACON_COUNT     3

/// Individual IR LED channel config.
typedef struct {
    uint8_t     gpio;
    uint8_t     phase_deg;   // 0, 120, or 240
    rmt_channel_handle_t tx_chan;
    rmt_encoder_handle_t encoder;
} ir_beacon_channel_t;

/// Beacon controller managing all 3 IR channels.
typedef struct {
    ir_beacon_channel_t channels[IR_BEACON_COUNT];
    uint8_t    station_id;
    bool       active;
} ir_beacon_t;

/// Initialize all 3 IR beacon channels with RMT (38kHz carrier + PPM).
void ir_beacon_init(ir_beacon_t *beacon, uint8_t station_id,
                    uint8_t gpio_a, uint8_t gpio_b, uint8_t gpio_c);

/// Start broadcasting the beacon signal.
void ir_beacon_start(ir_beacon_t *beacon);

/// Stop broadcasting (e.g. when robot docked, to save power).
void ir_beacon_stop(ir_beacon_t *beacon);

/// Build the PPM-encoded symbol for one bit (1 or 0).
/// Returns the RMT symbol pair for the 38kHz carrier burst + gap.
void ir_beacon_encode_bit(bool bit, rmt_symbol_word_t symbols[2]);

#ifdef __cplusplus
}
#endif
