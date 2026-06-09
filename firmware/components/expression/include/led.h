/**
 * LED driver — WS2812B RGB LEDs (8x) via RMT.
 *
 * Patterns (from BCP protocol):
 *   - Solid color
 *   - Breathing (sine fade)
 *   - Blinking
 *   - Rainbow cycle
 *   - Knight Rider (bounce)
 *   - Party (random flash)
 *
 * Used for: status indication, mood expression, visual feedback.
 */

#ifndef LED_H
#define LED_H

#include "freertos/FreeRTOS.h"
#include "bcp_codec.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** LED color (8-bit per channel). */
typedef struct {
    uint8_t r, g, b;
} led_color_t;

/** LED pattern command. */
typedef struct {
    bcp_led_mode_t mode;
    uint8_t        speed;    /* 0–255 */
    led_color_t    color;
} led_pattern_t;

/** Initialize WS2812B RMT driver. */
void led_init(void);

/** Set all LEDs to a solid color. */
void led_set_solid(led_color_t color);

/** Run a pattern. */
void led_set_pattern(led_pattern_t pattern);

/** Turn all LEDs off. */
void led_off(void);

/** LED control task (Core 1). */
void task_led(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* LED_H */
