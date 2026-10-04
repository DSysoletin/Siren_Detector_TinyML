#pragma once

// Siren Detector - hardcoded network configuration.
// Fill in the real values below before flashing. The placeholder values are
// intentionally invalid; with them the device will not connect to anything.
// NOTE: these credentials are stored in plaintext in the source tree.

#define WIFI_SSID         "YOUR_WIFI_SSID"
#define WIFI_PASS         "YOUR_WIFI_PASSWD"

#define MQTT_BROKER_HOST  "127.0.0.1"
#define MQTT_BROKER_PORT  1883
#define MQTT_CLIENT_ID    "siren-detector-01"
#define MQTT_USER         "REPLACE_ME_MQTT_USER"
#define MQTT_PASS         "REPLACE_ME_MQTT_PASS"
#define MQTT_STATUS_TOPIC "siren_detector_tinyml/status"
