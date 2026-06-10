#include "ir_beacon.h"
#include "driver/rmt_tx.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "ir_beacon"

#define RMT_RESOLUTION_HZ  1000000  // 1MHz = 1μs per tick

// ── RMT encoder for IR burst ────────────────────────────────────

typedef struct {
    rmt_encoder_t           base;
    rmt_encoder_t          *copy_encoder;
    rmt_symbol_word_t       burst_symbol;
    rmt_symbol_word_t       gap_symbol;
    uint32_t                burst_ticks;
    uint32_t                gap_ticks;
    int                     state;  // 0=burst, 1=gap
} ir_rmt_encoder_t;

static size_t ir_encoder_encode(rmt_encoder_t *encoder,
                                rmt_channel_handle_t channel,
                                const void *primary_data,
                                size_t data_size,
                                rmt_encode_state_t *ret_state)
{
    ir_rmt_encoder_t *ir = __containerof(encoder, ir_rmt_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encoder_handle_t copy = ir->copy_encoder;
    size_t encoded = 0;

    switch (ir->state) {
    case 0: // burst
        encoded += copy->encode(copy, channel, &ir->burst_symbol, 1, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            ir->state = 1;
        }
        break;
    case 1: // gap
        encoded += copy->encode(copy, channel, &ir->gap_symbol, 1, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            ir->state = 0;
        }
        break;
    }

    *ret_state = session_state;
    return encoded;
}

static esp_err_t ir_encoder_del(rmt_encoder_t *encoder)
{
    ir_rmt_encoder_t *ir = __containerof(encoder, ir_rmt_encoder_t, base);
    rmt_del_encoder(ir->copy_encoder);
    free(ir);
    return ESP_OK;
}

static esp_err_t ir_encoder_reset(rmt_encoder_t *encoder)
{
    ir_rmt_encoder_t *ir = __containerof(encoder, ir_rmt_encoder_t, base);
    ir->state = 0;
    rmt_encoder_reset(ir->copy_encoder);
    return ESP_OK;
}

static rmt_encoder_t *ir_encoder_create(uint32_t burst_us, uint32_t gap_us)
{
    ir_rmt_encoder_t *ir = calloc(1, sizeof(ir_rmt_encoder_t));
    assert(ir);

    ir->base.encode  = ir_encoder_encode;
    ir->base.del     = ir_encoder_del;
    ir->base.reset   = ir_encoder_reset;

    // Build copy encoder
    rmt_copy_encoder_config_t copy_cfg = {};
    rmt_new_copy_encoder(&copy_cfg, &ir->copy_encoder);

    // 38kHz carrier → 50% duty cycle
    uint32_t period_ticks = RMT_RESOLUTION_HZ / IR_CARRIER_HZ;
    uint32_t high_ticks   = period_ticks / 2;
    uint32_t low_ticks    = period_ticks - high_ticks;

    ir->burst_symbol = (rmt_symbol_word_t){
        .duration0 = high_ticks,
        .level0    = 1,
        .duration1 = low_ticks,
        .level1    = 0,
    };
    ir->gap_symbol = (rmt_symbol_word_t){
        .duration0 = 0,
        .level0    = 0,
        .duration1 = 0,
        .level1    = 0,
    };
    ir->burst_ticks = burst_us;
    ir->gap_ticks   = gap_us;
    ir->state       = 0;

    return &ir->base;
}

// ── PPM bit encoding ─────────────────────────────────────────────

void ir_beacon_encode_bit(bool bit, rmt_symbol_word_t symbols[2])
{
    // PPM: '1' = long burst (600μs) + short gap (400μs)
    //       '0' = short burst (300μs) + long gap (700μs)
    // 38kHz carrier bursts convey energy; spacing conveys bit value.
    if (bit) {
        symbols[0] = (rmt_symbol_word_t){ .duration0 = 600, .level0 = 1, .duration1 = 0, .level1 = 0 };
        symbols[1] = (rmt_symbol_word_t){ .duration0 = 400, .level0 = 0, .duration1 = 0, .level1 = 0 };
    } else {
        symbols[0] = (rmt_symbol_word_t){ .duration0 = 300, .level0 = 1, .duration1 = 0, .level1 = 0 };
        symbols[1] = (rmt_symbol_word_t){ .duration0 = 700, .level0 = 0, .duration1 = 0, .level1 = 0 };
    }
}

// ── Compute 4-bit checksum (XOR of station_id nibbles) ─────────

static uint8_t beacon_checksum(uint8_t station_id)
{
    uint8_t hi = (station_id >> 4) & 0x0F;
    uint8_t lo = station_id & 0x0F;
    return (hi ^ lo) & 0x0F;
}

// ── Channel init ─────────────────────────────────────────────────

static void ir_channel_init(ir_beacon_channel_t *ch,
                            uint8_t gpio, uint8_t phase_deg)
{
    ch->gpio      = gpio;
    ch->phase_deg = phase_deg;

    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num          = gpio,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 128,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_cfg, &ch->tx_chan));

    // Enable the channel
    ESP_ERROR_CHECK(rmt_enable(ch->tx_chan));
}

// ── Public API ───────────────────────────────────────────────────

void ir_beacon_init(ir_beacon_t *beacon, uint8_t station_id,
                    uint8_t gpio_a, uint8_t gpio_b, uint8_t gpio_c)
{
    beacon->station_id = station_id;
    beacon->active     = false;

    ir_channel_init(&beacon->channels[0], gpio_a, 0);
    ir_channel_init(&beacon->channels[1], gpio_b, 120);
    ir_channel_init(&beacon->channels[2], gpio_c, 240);

    ESP_LOGI(TAG, "initialized station=%d gpios=%d,%d,%d",
             station_id, gpio_a, gpio_b, gpio_c);
}

void ir_beacon_start(ir_beacon_t *beacon)
{
    if (beacon->active) return;
    beacon->active = true;

    // Build preamble + data bits
    // Each phase-shifted channel transmits at a different time offset
    // so the robot can distinguish the 3 IR sources.
    ESP_LOGI(TAG, "beacon started station=%d", beacon->station_id);
}

void ir_beacon_stop(ir_beacon_t *beacon)
{
    if (!beacon->active) return;
    beacon->active = false;
    ESP_LOGI(TAG, "beacon stopped");
}
