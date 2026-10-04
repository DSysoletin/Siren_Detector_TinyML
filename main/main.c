#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_capture.h"
#include "siren_model.h"
#include "siren_net.h"
#include "siren_preprocess.h"

static const char *TAG = "siren_detector";

#define INFER_CYCLE_MIN_US (300 * 1000)  // min period between cycle starts
#define DETECT_THRESHOLD 0.97f           // PC TFLite threshold (visibility only)

static void siren_infer_task(void *arg)
{
    // One cycle of scratch. The 96 KB window and 36.4 KB tensor are
    // function-scope statics (internal DRAM); the spectrogram/sort scratch is
    // allocated inside siren_preprocess_run (PSRAM-first).
    static float window[SIREN_WIN_SAMPLES];
    static uint8_t tensor[SIREN_TENSOR_SIZE];

    // Wait for the rolling buffer to fill (first 3 s of capture).
    while (!siren_audio_snapshot(window)) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "buffer full, starting inference cycles");

    for (;;) {
        int64_t t0 = esp_timer_get_time();

        if (siren_audio_snapshot(window) &&
            siren_preprocess_run(window, tensor) == 0) {
            int64_t t_pp = esp_timer_get_time();  // end of preprocess
            uint8_t out = 0;
            int64_t dt_infer = 0;
            if (siren_model_invoke(tensor, &out, &dt_infer)) {
                int64_t t_end = esp_timer_get_time();  // end of model
                float score = (float)out / 255.0f;
                uint8_t detected = (score >= DETECT_THRESHOLD) ? 1 : 0;
                const char *flag = detected ? "  DETECT" : "";
                ESP_LOGI(TAG,
                         "score=%.6f  preprocess=%.2fms  model=%.2fms  cycle=%.2fms%s",
                         (double)score, (double)(t_pp - t0) / 1000.0,
                         (double)(t_end - t_pp) / 1000.0,
                         (double)(t_end - t0) / 1000.0, flag);
                siren_net_report(detected);
            } else {
                ESP_LOGE(TAG, "model invoke failed");
            }
        } else {
            ESP_LOGW(TAG, "snapshot/preprocess failed, skipping cycle");
        }

        // Pace to at least INFER_CYCLE_MIN_US between cycle starts (mirrors the
        // PC detector's `while (t - start) < 300: sleep`).
        int64_t elapsed = esp_timer_get_time() - t0;
        if (elapsed < INFER_CYCLE_MIN_US) {
            vTaskDelay(pdMS_TO_TICKS((INFER_CYCLE_MIN_US - elapsed) / 1000));
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Siren Detector (ESP32-P4) starting");
    // Network is non-fatal: if WiFi/MQTT init fails the detection pipeline
    // keeps running and the console reporting keeps working.
    if (!siren_net_init()) {
        ESP_LOGE(TAG, "network init failed; continuing without WiFi/MQTT");
    }
    vTaskDelay(5000 / portTICK_PERIOD_MS);
    if (!siren_model_init()) {
        ESP_LOGE(TAG, "model init failed, halting");
        vTaskDelay(pdMS_TO_TICKS(10000));
        return;
    }
    siren_model_run_synthetic_test();

    if (siren_preprocess_init() != 0) {
        ESP_LOGE(TAG, "preprocess init failed, halting");
        vTaskDelay(pdMS_TO_TICKS(10000));
        return;
    }
    float peak = 0.0f;
    if (siren_preprocess_selftest(&peak)) {
        ESP_LOGI(TAG, "fft self-test: PASS (peak bin mag=%.2f, expect ~%.0f)",
                 peak, (double)SIREN_FFT_SIZE / 2.0);
    } else {
        ESP_LOGE(TAG, "fft self-test: FAIL (peak bin mag=%.2f, expect ~%.0f)",
                 peak, (double)SIREN_FFT_SIZE / 2.0);
    }

    if (siren_audio_init() != 0) {
        ESP_LOGE(TAG, "audio init failed, halting");
        vTaskDelay(pdMS_TO_TICKS(10000));
        return;
    }
    if (siren_audio_start_task() != 0) {
        ESP_LOGE(TAG, "audio task start failed, halting");
        vTaskDelay(pdMS_TO_TICKS(10000));
        return;
    }

    if (xTaskCreatePinnedToCore(siren_infer_task, "siren_infer", 16384, NULL, 5,
                                NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "siren_infer task create failed, halting");
        vTaskDelay(pdMS_TO_TICKS(10000));
        return;
    }
    ESP_LOGI(TAG, "siren_infer task started (core 1, prio 5); pipeline live");


}
