/**
 * Face display — SSD1306 OLED (128x64) over I2C.
 *
 * Renders facial expressions as bitmap icons:
 *   - Happy, Sad, Angry, Surprised, Neutral
 *   - Blinking animation (random interval 2-6s)
 *   - Custom bitmap upload via BCP
 *
 * Expression transitions use dissolve effect (200ms).
 */

#ifndef FACE_H
#define FACE_H

#include "freertos/FreeRTOS.h"
#include "bcp_codec.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize SSD1306 OLED display. */
void face_init(void);

/** Set facial expression. */
void face_set_expression(bcp_face_expr_t expr);

/** Set custom bitmap (via BCP CMD_FACE_CUSTOM). */
void face_set_custom(const uint8_t *bitmap, size_t len);

/** Turn off display (sleep). */
void face_off(void);

#ifdef __cplusplus
}
#endif

#endif /* FACE_H */
