#pragma once

#include <stdint.h>

// Pipeline constants (must match the trained model / PC reference).
#define SIREN_WIN_SAMPLES 24000
#define SIREN_FRAME_LEN 192
#define SIREN_HOP 64
#define SIREN_FFT_SIZE 256
#define SIREN_NUM_FRAMES 373
#define SIREN_NUM_BINS 100
#define SIREN_TENSOR_SIZE (SIREN_NUM_FRAMES * SIREN_NUM_BINS)

#ifdef __cplusplus
extern "C" {
#endif

// Initialise the FFT backend (twiddle tables on ESP-DSP; no-op on host).
// Must be called once before siren_preprocess_run(). Returns 0 on success.
int siren_preprocess_init(void);

// Convert a SIREN_WIN_SAMPLES-sample float32 window in [-1, 1) into the model
// uint8 input tensor (1, 373, 100, 1) via Hann-windowed spectrogram (first
// 100 bins of a 256-pt FFT, log magnitude) + 1st/99th percentile uint8
// quantization. `out` must hold at least SIREN_TENSOR_SIZE bytes.
// Returns 0 on success, -1 on allocation failure.
int siren_preprocess_run(const float *window, uint8_t *out);

// Validate the FFT backend: synthesise a cosine at bin 10, run one frame, and
// check that bin 10 carries ~N/2 magnitude while neighbours are near zero.
// If `peak_out` is non-NULL it receives the measured magnitude at the expected
// peak bin (expect ~SIREN_FFT_SIZE / 2). Returns 1 on pass, 0 on fail.
int siren_preprocess_selftest(float *peak_out);

#ifdef __cplusplus
}
#endif
