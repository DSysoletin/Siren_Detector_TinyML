#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SIREN_INPUT_SIZE (373 * 100)
#define SIREN_OUTPUT_SIZE 1

#ifdef __cplusplus
extern "C" {
#endif

bool siren_model_init(void);
bool siren_model_invoke(const uint8_t *input_data, uint8_t *output, int64_t *dt_us);
void siren_model_run_synthetic_test(void);

#ifdef __cplusplus
}
#endif
