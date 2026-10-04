#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "siren_preprocess.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialise the ES8311 codec + I2S capture path (via the Waveshare ESP32-P4
// BSP) at 8 kHz / 16-bit and prepare the rolling buffer. Must be called once
// before siren_audio_start_task(). Returns 0 on success, non-zero on failure.
int siren_audio_init(void);

// Start the audio_cap FreeRTOS task (core 0, priority 6, 6 KB stack) that
// continuously reads the codec, mixes L/R to mono, and appends to the ring.
// Returns 0 on success, non-zero on failure.
int siren_audio_start_task(void);

// Copy the most recent SIREN_WIN_SAMPLES (24000) captured samples, oldest
// first, into `out` (a float buffer of SIREN_WIN_SAMPLES), scaled to [-1, 1)
// (int16 / 32768.0) - the exact float window the preprocessing consumes.
// Returns true once the ring is full and `out` has been written; false
// otherwise (`out` is left unmodified).
bool siren_audio_snapshot(float *out);

#ifdef __cplusplus
}
#endif
