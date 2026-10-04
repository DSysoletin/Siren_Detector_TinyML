#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mqtt_client.h"

#include "siren_config.h"
#include "siren_net.h"

static const char *TAG = "siren_net";

#define WIFI_RETRY_DELAY_MS 2000
#define MQTT_RECONNECT_TIMEOUT_MS 10000
#define DETECTION_CONFIRM_COUNT 5  // consecutive positive model runs required to report a detection

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static uint8_t s_last_reported = 0;  // last status published to the broker
static uint32_t s_consecutive_positives = 0; // consecutive positive model runs so far

static void s_publish_status(uint8_t status)
{
    if (s_mqtt_client == NULL) {
        return;
    }
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, MQTT_STATUS_TOPIC,
                                         status ? "1" : "0", 1, 1, 1);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "status publish failed (msg_id=%d)", msg_id);
    }
}

static void s_mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                 int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "mqtt connected");
        s_publish_status(s_last_reported);  // baseline on (re)connect
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "mqtt disconnected, auto-reconnect active");
        break;
    case MQTT_EVENT_ERROR:
        if (event->error_handle) {
            ESP_LOGE(TAG, "mqtt error (type=%d, conn_rc=%d, msg_id=%d)",
                     (int)event->error_handle->error_type,
                     (int)event->error_handle->connect_return_code,
                     event->msg_id);
        } else {
            ESP_LOGE(TAG, "mqtt error (msg_id=%d)", event->msg_id);
        }
        break;
    default:
        break;
    }
}

static void s_wifi_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *dis =
            (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "wifi disconnected (reason=%d), retrying", dis->reason);
        vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_DELAY_MS));
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *got = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "wifi got IP: " IPSTR, IP2STR(&got->ip_info.ip));
    }
}

bool siren_net_init(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s",
                 esp_err_to_name(err));
        return false;
    }

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return false;
    }

    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        return false;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              s_wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi event handler register failed: %s",
                 esp_err_to_name(err));
        return false;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              s_wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ip event handler register failed: %s",
                 esp_err_to_name(err));
        return false;
    }

    wifi_config_t sta_cfg = {
        .sta = {
            .pmf_cfg = { .capable = true, .required = false },
        },
    };
    strncpy((char *)sta_cfg.sta.ssid, WIFI_SSID, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, WIFI_PASS,
            sizeof(sta_cfg.sta.password) - 1);

    esp_wifi_set_mode(WIFI_MODE_STA);
    if (esp_wifi_set_config(WIFI_IF_STA, &sta_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed");
        return false;
    }
    if (esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed");
        return false;
    }
    ESP_LOGI(TAG, "wifi starting (ssid=%s)", WIFI_SSID);

    char uri[128];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", MQTT_BROKER_HOST,
             MQTT_BROKER_PORT);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address = { .uri = uri, .port = MQTT_BROKER_PORT },
        .credentials = {
            .client_id = MQTT_CLIENT_ID,
            .username = MQTT_USER,
            .authentication = { .password = MQTT_PASS },
        },
        .session = { .protocol_ver = MQTT_PROTOCOL_V_3_1_1 },
        .network = {
            .reconnect_timeout_ms = MQTT_RECONNECT_TIMEOUT_MS,
            .timeout_ms = 30000,
        },
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return false;
    }
    if (esp_mqtt_client_register_event(s_mqtt_client, MQTT_EVENT_ANY,
                                       s_mqtt_event_handler, NULL) !=
        ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_register_event failed");
        return false;
    }
    if (esp_mqtt_client_start(s_mqtt_client) != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start failed");
        return false;
    }
    ESP_LOGI(TAG, "mqtt client started (uri=%s, client_id=%s)", uri,
             MQTT_CLIENT_ID);
    return true;
}

void siren_net_report(uint8_t detected)
{
    uint8_t status = detected ? 1 : 0;
    uint8_t status_to_report = 0;

    if (status == 1) {
        s_consecutive_positives++;
    } else {
        s_consecutive_positives = 0;
    }
    if (s_consecutive_positives >= DETECTION_CONFIRM_COUNT) {
        status_to_report = 1;
    }

    if (status_to_report == s_last_reported) {
        return;
    }

    s_last_reported = status_to_report;
    ESP_LOGI(TAG, "detection status reporting -> %d", status_to_report);
    s_publish_status(status_to_report);
}
