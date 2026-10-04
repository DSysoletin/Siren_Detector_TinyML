#include "siren_model.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"

extern "C" {
extern const uint8_t siren_detector_model_tflite[];
extern const int siren_detector_model_tflite_length;
}

static const char *TAG = "siren_model";

namespace {

constexpr size_t kArenaSize = 300 * 1024;

const tflite::Model *model = nullptr;
tflite::MicroInterpreter *interpreter = nullptr;
TfLiteTensor *input = nullptr;
TfLiteTensor *output = nullptr;
uint8_t *arena = nullptr;

void log_tensor(const char *label, const TfLiteTensor *t)
{
    char shape[64];
    int off = 0;
    for (int i = 0; i < t->dims->size && off < (int)sizeof(shape) - 4; i++) {
        off += snprintf(shape + off, sizeof(shape) - off, "%s%d", i ? ", " : "", t->dims->data[i]);
    }
    ESP_LOGI(TAG, "%s tensor: type=%s bytes=%u shape=[%s]",
             label, TfLiteTypeGetName(t->type), (unsigned)t->bytes, shape);
}

}

extern "C" bool siren_model_init(void)
{
    model = tflite::GetModel(siren_detector_model_tflite);
    if (model == nullptr) {
        ESP_LOGE(TAG, "tflite::GetModel failed");
        return false;
    }
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "model schema version %d != supported %d",
                 (int)model->version(), (int)TFLITE_SCHEMA_VERSION);
        return false;
    }
    ESP_LOGI(TAG, "model loaded: %d bytes, schema %d",
             (int)siren_detector_model_tflite_length, (int)model->version());

    static tflite::MicroMutableOpResolver<7> resolver;
    TfLiteStatus status = resolver.AddQuantize();
    if (status == kTfLiteOk) status = resolver.AddResizeBilinear();
    if (status == kTfLiteOk) status = resolver.AddConv2D();
    if (status == kTfLiteOk) status = resolver.AddMaxPool2D();
    if (status == kTfLiteOk) status = resolver.AddMean();
    if (status == kTfLiteOk) status = resolver.AddFullyConnected();
    if (status == kTfLiteOk) status = resolver.AddLogistic();
    if (status != kTfLiteOk) {
        ESP_LOGE(TAG, "op registration failed (%d)", (int)status);
        return false;
    }

    arena = static_cast<uint8_t *>(heap_caps_malloc(kArenaSize, MALLOC_CAP_SPIRAM));
    bool arena_psram = arena != nullptr;
    if (arena == nullptr) {
        arena = static_cast<uint8_t *>(heap_caps_malloc(kArenaSize, MALLOC_CAP_INTERNAL));
    }
    if (arena == nullptr) {
        ESP_LOGE(TAG, "tensor arena alloc failed: %u KB",
                 (unsigned)(kArenaSize / 1024));
        return false;
    }
    ESP_LOGI(TAG, "tensor arena: %u KB in %s",
             (unsigned)(kArenaSize / 1024), arena_psram ? "PSRAM" : "internal SRAM");

    static tflite::MicroInterpreter interp(model, resolver, arena, kArenaSize);
    interpreter = &interp;

    status = interpreter->AllocateTensors();
    if (status != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed (%d)", (int)status);
        return false;
    }

    input = interpreter->input(0);
    output = interpreter->output(0);
    if (input == nullptr || output == nullptr) {
        ESP_LOGE(TAG, "input/output tensor lookup failed");
        return false;
    }
    log_tensor("input", input);
    log_tensor("output", output);
    ESP_LOGI(TAG, "model ready");
    return true;
}

extern "C" bool siren_model_invoke(const uint8_t *input_data, uint8_t *output, int64_t *dt_us)
{
    if (interpreter == nullptr || input == nullptr || output == nullptr) {
        return false;
    }
    if (input_data != nullptr) {
        memcpy(input->data.uint8, input_data, SIREN_INPUT_SIZE);
    }
    int64_t t0 = esp_timer_get_time();
    TfLiteStatus status = interpreter->Invoke();
    int64_t dt = esp_timer_get_time() - t0;
    if (dt_us != nullptr) {
        *dt_us = dt;
    }
    if (status != kTfLiteOk) {
        ESP_LOGE(TAG, "Invoke failed (%d)", (int)status);
        return false;
    }
    if (output != nullptr) {
        memcpy(output, interpreter->output(0)->data.uint8, SIREN_OUTPUT_SIZE);
    }
    return true;
}

extern "C" void siren_model_run_synthetic_test(void)
{
    if (interpreter == nullptr || input == nullptr) {
        ESP_LOGE(TAG, "model not initialized");
        return;
    }
    for (int i = 0; i < SIREN_INPUT_SIZE; i++) {
        input->data.uint8[i] = (uint8_t)((i * 7 + 13) & 0xFF);
    }
    uint8_t out[SIREN_OUTPUT_SIZE];
    int64_t dt_us = 0;
    if (!siren_model_invoke(nullptr, out, &dt_us)) {
        ESP_LOGE(TAG, "synthetic Invoke failed");
        return;
    }
    ESP_LOGI(TAG, "synthetic test: output=%u score=%.6f time=%lld us",
             (unsigned)out[0], (double)out[0] / 255.0, (long long)dt_us);
}
