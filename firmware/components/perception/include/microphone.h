/**
 * Microphone driver — INMP441 4ch array over I2S.
 *
 * Captures 16kHz 16-bit audio for:
 *   - Voice command detection
 *   - Sound source localization (4ch beamforming)
 *   - Audio event detection (clap, crash, etc.)
 */

#ifndef MICROPHONE_H
#define MICROPHONE_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Audio chunk (queued to audio processing task). */
typedef struct {
    int16_t *samples;
    size_t   sample_count;
    uint8_t  channels;
    uint32_t timestamp_ms;
} audio_chunk_t;

/** Initialize I2S microphone array. */
void microphone_init(void);

/** Audio input task (Core 1). */
void task_audio_in(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* MICROPHONE_H */
