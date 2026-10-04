#include "audio_capture.h"

#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "siren_audio";

#define CAP_RATE_HZ 8000
#define CAP_CHANNELS 2  // capture stereo (L/R) and mix down to mono
#define CAP_CHUNK_FRAMES 640  // 80 ms at 8 kHz

// 24000-sample (3 s @ 8 kHz) int16 rolling buffer in internal RAM.
static int16_t s_ring[SIREN_WIN_SAMPLES];
static int s_head = 0;    // next write index
static int s_count = 0;   // valid samples, capped at SIREN_WIN_SAMPLES
static SemaphoreHandle_t s_mutex = NULL;

static esp_codec_dev_handle_t s_record_dev = NULL;

// Capture scratch (file scope to keep them off the task stack).
static int16_t s_stereo[CAP_CHUNK_FRAMES * CAP_CHANNELS];
static int16_t s_mono[CAP_CHUNK_FRAMES];

static void ring_append_locked(const int16_t *samples, int n)
{
    for (int i = 0; i < n; i++) {
        s_ring[s_head] = samples[i];
        s_head = (s_head + 1) % SIREN_WIN_SAMPLES;
        if (s_count < SIREN_WIN_SAMPLES) {
            s_count++;
        }
    }
}

static void audio_cap_task(void *arg)
{
    bool logged_first = false;
    for (;;) {
        int ret = esp_codec_dev_read(s_record_dev, s_stereo,
                                     sizeof(s_stereo));
        if (ret != ESP_CODEC_DEV_OK) {
            // Log and recover rather than abort: the ring keeps its last
            // good data and the next cycle proceeds.
            ESP_LOGW(TAG, "codec read failed (%d), retrying", ret);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Mix interleaved L/R down to mono: (L + R) / 2, matching the
        // multi->mono convention in tinyML/wav.c (sum / num_channels).
        int frames = CAP_CHUNK_FRAMES;
        int peak = 0;
        for (int i = 0; i < frames; i++) {
            int32_t l = s_stereo[2 * i];
            int32_t r = s_stereo[2 * i + 1];
            int16_t m = (int16_t)((l + r) / 2);
            s_mono[i] = m;
            int a = (m < 0) ? -m : m;
            if (a > peak) {
                peak = a;
            }
        }

        if (!logged_first) {
            logged_first = true;
            ESP_LOGI(TAG, "first capture chunk ok: %d frames, peak int16=%d",
                     frames, peak);
        }

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        ring_append_locked(s_mono, frames);
        xSemaphoreGive(s_mutex);
    }
}

int siren_audio_init(void)
{
    s_record_dev = bsp_audio_codec_microphone_init();
    if (s_record_dev == NULL) {
        ESP_LOGE(TAG, "microphone codec init failed");
        return -1;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = CAP_RATE_HZ,
        .channel = CAP_CHANNELS,
        .bits_per_sample = 16,
        .mclk_multiple = 0,  // 256x sample rate
    };
    if (esp_codec_dev_set_in_gain(s_record_dev, 24.0) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "set input gain failed (continuing)");
    }
    if (esp_codec_dev_open(s_record_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "open record device failed");
        return -1;
    }
    ESP_LOGI(TAG, "codec ready: %u Hz, %d-bit, %d ch (mono-mixed)",
             (unsigned)CAP_RATE_HZ, 16, CAP_CHANNELS);

    s_head = 0;
    s_count = 0;
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "mutex create failed");
        return -1;
    }
    return 0;
}

int siren_audio_start_task(void)
{
    if (s_record_dev == NULL || s_mutex == NULL) {
        return -1;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(audio_cap_task, "audio_cap", 6144,
                                            NULL, 6, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "audio_cap task create failed");
        return -1;
    }
    ESP_LOGI(TAG, "audio_cap task started (core 0, prio 6)");
    return 0;
}

bool siren_audio_snapshot(float *out)
{
    if (out == NULL) {
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool full = (s_count >= SIREN_WIN_SAMPLES);
    if (full) {
        // When full, the oldest sample sits at s_head.
        int start = s_head;
        for (int i = 0; i < SIREN_WIN_SAMPLES; i++) {
            out[i] = (float)s_ring[(start + i) % SIREN_WIN_SAMPLES] / 32768.0f;
        }
    }
    xSemaphoreGive(s_mutex);
    return full;
}
