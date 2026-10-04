#include "siren_preprocess.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------------------
// FFT backend + allocator selection.
// On the ESP32-P4 we use ESP-DSP's radix-2 complex FFT; on a host build (no
// CONFIG_IDF_TARGET) we fall back to the reference fft_inplace() so the same
// source can be compiled for the parity check.
// ---------------------------------------------------------------------------
#if defined(CONFIG_IDF_TARGET)

#include "esp_dsp.h"
#include "esp_heap_caps.h"

static void *sp_alloc(size_t n) {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_DEFAULT);
    if (!p) {
        p = malloc(n);
    }
    return p;
}

static void sp_free(void *p) {
    if (p) {
        heap_caps_free(p);
    }
}

static int fft_backend_init(void) {
    esp_err_t ret = dsps_fft2r_init_fc32(NULL, CONFIG_DSP_MAX_FFT_SIZE);
    return (ret == ESP_OK) ? 0 : -1;
}

static void fft_backend_run(float *re, float *im, int n) {
    static float data[2 * SIREN_FFT_SIZE];
    for (int i = 0; i < n; i++) {
        data[2 * i] = re[i];
        data[2 * i + 1] = im[i];
    }
    dsps_fft2r_fc32(data, n);
    dsps_bit_rev_fc32(data, n);
    for (int i = 0; i < n; i++) {
        re[i] = data[2 * i];
        im[i] = data[2 * i + 1];
    }
}

#else

#include "fft.h"

static void *sp_alloc(size_t n) { return malloc(n); }

static void sp_free(void *p) { free(p); }

static int fft_backend_init(void) { return 0; }

static void fft_backend_run(float *re, float *im, int n) {
    fft_inplace(re, im, n);
}

#endif

// ---------------------------------------------------------------------------
// Spectrogram: Hann(192) window, hop 64, zero-padded 256-pt FFT, first 100
// bins, log magnitude. Ported verbatim (numerics) from tinyML/siren_tinyml.c.
// ---------------------------------------------------------------------------
static void compute_spectrogram(const float *window, float *spec) {
    static float hann[SIREN_FRAME_LEN];
    static int hann_ready = 0;
    if (!hann_ready) {
        for (int i = 0; i < SIREN_FRAME_LEN; i++) {
            hann[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i /
                                          (float)SIREN_FRAME_LEN));
        }
        hann_ready = 1;
    }

    static float re[SIREN_FFT_SIZE];
    static float im[SIREN_FFT_SIZE];
    for (int f = 0; f < SIREN_NUM_FRAMES; f++) {
        int start = f * SIREN_HOP;
        for (int i = 0; i < SIREN_FRAME_LEN; i++) {
            int idx = start + i;
            float x = (idx < SIREN_WIN_SAMPLES) ? window[idx] : 0.0f;
            re[i] = x * hann[i];
            im[i] = 0.0f;
        }
        for (int i = SIREN_FRAME_LEN; i < SIREN_FFT_SIZE; i++) {
            re[i] = 0.0f;
            im[i] = 0.0f;
        }
        fft_backend_run(re, im, SIREN_FFT_SIZE);
        for (int b = 0; b < SIREN_NUM_BINS; b++) {
            float mag = sqrtf(re[b] * re[b] + im[b] * im[b]);
            spec[f * SIREN_NUM_BINS + b] = logf(mag + 1e-6f);
        }
    }
}

static int cmp_float(const void *a, const void *b) {
    float fa = *(const float *)a;
    float fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

// ---------------------------------------------------------------------------
// Percentile uint8 quantization. Ported verbatim (numerics) from
// tinyML/siren_tinyml.c: qsort of all 37300 floats, numpy-compatible linear
// interpolation of the 1st/99th percentiles in float64, clipping, then
// (x - lo) / (hi - lo + 1e-8f) * 255.0f with a truncating cast.
// ---------------------------------------------------------------------------
static void quantize_percentile_uint8(const float *spec, uint8_t *out) {
    int n = SIREN_TENSOR_SIZE;
    float *sorted = sp_alloc((size_t)n * sizeof(float));
    if (!sorted) {
        return;
    }
    memcpy(sorted, spec, (size_t)n * sizeof(float));
    qsort(sorted, (size_t)n, sizeof(float), cmp_float);

    float lo32 = 0.0f;
    float hi32 = 0.0f;
    for (int which = 0; which < 2; which++) {
        int p = which ? 99 : 1;
        double pos = (double)p / 100.0 * (double)(n - 1);
        int idx = (int)pos;
        if (idx > n - 2) {
            idx = n - 2;
        }
        double frac = pos - (double)idx;
        double v = (double)sorted[idx] +
                   ((double)sorted[idx + 1] - (double)sorted[idx]) * frac;
        if (which) {
            hi32 = (float)v;
        } else {
            lo32 = (float)v;
        }
    }

    float denom = hi32 - lo32 + 1e-8f;
    for (int i = 0; i < n; i++) {
        float x = spec[i];
        if (x < lo32) {
            x = lo32;
        }
        if (x > hi32) {
            x = hi32;
        }
        float scaled = (x - lo32) / denom * 255.0f;
        out[i] = (uint8_t)scaled;
    }
    sp_free(sorted);
}

int siren_preprocess_init(void) {
    return fft_backend_init();
}

int siren_preprocess_run(const float *window, uint8_t *out) {
    if (!window || !out) {
        return -1;
    }
    float *spec = sp_alloc((size_t)SIREN_TENSOR_SIZE * sizeof(float));
    if (!spec) {
        return -1;
    }
    compute_spectrogram(window, spec);
    quantize_percentile_uint8(spec, out);
    sp_free(spec);
    return 0;
}

int siren_preprocess_selftest(float *peak_out) {
    const int K = 10;
    static float re[SIREN_FFT_SIZE];
    static float im[SIREN_FFT_SIZE];
    for (int n = 0; n < SIREN_FFT_SIZE; n++) {
        re[n] =
            cosf(2.0f * (float)M_PI * (float)K * (float)n /
                 (float)SIREN_FFT_SIZE);
        im[n] = 0.0f;
    }
    fft_backend_run(re, im, SIREN_FFT_SIZE);

    float peak = sqrtf(re[K] * re[K] + im[K] * im[K]);
    float o1 = sqrtf(re[K - 1] * re[K - 1] + im[K - 1] * im[K - 1]);
    float o2 = sqrtf(re[K + 1] * re[K + 1] + im[K + 1] * im[K + 1]);
    if (peak_out) {
        *peak_out = peak;
    }

    // A cosine exactly on bin K yields magnitude N/2 at bin K and ~0 at
    // adjacent bins for a correctly ordered, unnormalized forward FFT.
    int pass = (peak > 120.0f && peak < 136.0f) && (o1 < 2.0f) && (o2 < 2.0f);
    return pass;
}
