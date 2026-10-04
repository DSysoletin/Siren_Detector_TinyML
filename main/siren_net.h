#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the WiFi + MQTT stack (non-blocking). Returns false on a fatal
// init error; the caller MUST still run the detection pipeline.
bool siren_net_init(void);

// Report the current detection status. Publishes exactly "1" or "0" to the
// configured MQTT topic, only when the status differs from the last
// reported value. Safe to call from any task (no-op when unchanged).
void siren_net_report(uint8_t detected);

#ifdef __cplusplus
}
#endif
