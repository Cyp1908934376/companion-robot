/**
 * Speaker driver — MAX98357A I2S Class-D amplifier.
 *
 * Outputs:
 *   - Pre-recorded WAV samples (stored in SPIFFS)
 *   - TTS audio frames streamed via BCP
 *   - System alert tones (boot chime, low battery, error beep)
 *
 * 22.05kHz 16-bit mono. Double-buffered DMA.
 */

#ifndef SPEAKER_H
#define SPEAKER_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Audio output buffer. */
typedef struct {
    int16_t *samples;
    size_t   sample_count;
    bool     tts;   /* true = TTS stream, false = WAV file */
} audio_out_buf_t;

/** Initialize MAX98357A I2S output. */
void speaker_init(void);

/** Play a WAV file from SPIFFS by path. */
void speaker_play_file(const char *path);

/** Enqueue TTS samples for streaming playback. */
void speaker_enqueue_tts(const int16_t *samples, size_t count);

/** Audio output task (Core 1). */
void task_audio_out(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* SPEAKER_H */
