#include "tele_presence.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "firmware_version.h"
#include "tele_mqtt.h"
#include "tele_system_registry.h"
#include "time_sync.h"
#include "wifi_manager.h"

#ifndef CONFIG_MQTT_PRESENCE_ENABLED
#define CONFIG_MQTT_PRESENCE_ENABLED 0
#endif

#ifndef CONFIG_MQTT_BROKER_URI
#define CONFIG_MQTT_BROKER_URI ""
#endif

#ifndef CONFIG_MQTT_USERNAME
#define CONFIG_MQTT_USERNAME ""
#endif

#ifndef CONFIG_MQTT_PASSWORD
#define CONFIG_MQTT_PASSWORD ""
#endif

#ifndef CONFIG_MQTT_BASE_TOPIC
#define CONFIG_MQTT_BASE_TOPIC "v1/telesystem"
#endif

#ifndef CONFIG_MQTT_DEVICE_ID_PREFIX
#define CONFIG_MQTT_DEVICE_ID_PREFIX "ESP32-Device"
#endif

#ifndef CONFIG_MQTT_HEARTBEAT_INTERVAL_S
#define CONFIG_MQTT_HEARTBEAT_INTERVAL_S 60
#endif

#ifndef CONFIG_MQTT_KEEPALIVE_S
#define CONFIG_MQTT_KEEPALIVE_S 60
#endif

#ifndef CONFIG_MQTT_QOS_CRITICAL
#define CONFIG_MQTT_QOS_CRITICAL 1
#endif

#ifndef CONFIG_MQTT_QOS_TELEMETRY
#define CONFIG_MQTT_QOS_TELEMETRY 0
#endif

static const char *TAG = "tele-presence";

static bool s_started;
static bool s_wifi_event_registered;

static bool tele_presence_ready(void *ctx)
{
    wifi_manager_status_t wifi_status = {0};
    (void)ctx;

    return wifi_manager_get_status(&wifi_status) == ESP_OK &&
           wifi_status.state == WIFI_MANAGER_STATE_STA_CONNECTED &&
           wifi_status.wifi_ready &&
           time_sync_is_synchronized();
}

static bool tele_presence_build_timestamp(char *buffer, size_t buffer_len, void *ctx)
{
    (void)ctx;
    return time_sync_format_utc_now(buffer, buffer_len);
}

static cJSON *tele_presence_build_technical_status(void *ctx)
{
    (void)ctx;
    return tele_system_registry_build_technical_status();
}

static uint32_t tele_presence_get_heartbeat_interval_s(void *ctx)
{
    (void)ctx;
    return tele_mqtt_get_heartbeat_interval_s();
}

static esp_err_t tele_presence_apply_heartbeat_interval_s(uint32_t value, void *ctx)
{
    (void)ctx;
    return tele_mqtt_apply_heartbeat_interval_s(value);
}

static void tele_presence_restart(uint32_t delay_ms, void *ctx)
{
    (void)delay_ms;
    (void)ctx;
    esp_restart();
}

static void tele_presence_wifi_event_handler(void *arg,
                                             esp_event_base_t event_base,
                                             int32_t event_id,
                                             void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    if (event_id == WIFI_MANAGER_EVENT_STA_CONNECTED) {
        (void)tele_mqtt_start_client_if_ready();
    }
}

esp_err_t tele_presence_start(void)
{
#if !CONFIG_MQTT_PRESENCE_ENABLED
    return ESP_OK;
#else
    tele_mqtt_config_t mqtt_config = {
        .broker_uri = CONFIG_MQTT_BROKER_URI,
        .username = CONFIG_MQTT_USERNAME,
        .password = CONFIG_MQTT_PASSWORD,
        .base_topic = CONFIG_MQTT_BASE_TOPIC,
        .device_id_prefix = CONFIG_MQTT_DEVICE_ID_PREFIX,
        .firmware_version = APP_VERSION_STRING,
        .heartbeat_interval_s = CONFIG_MQTT_HEARTBEAT_INTERVAL_S,
        .keepalive_s = CONFIG_MQTT_KEEPALIVE_S,
        .qos_critical = CONFIG_MQTT_QOS_CRITICAL,
        .qos_telemetry = CONFIG_MQTT_QOS_TELEMETRY,
        .is_ready = tele_presence_ready,
        .build_timestamp = tele_presence_build_timestamp,
        .build_technical_status = tele_presence_build_technical_status,
        .restart = tele_presence_restart,
    };
    const tele_system_registry_config_t registry_config = {
        .heartbeat_interval_default_s = CONFIG_MQTT_HEARTBEAT_INTERVAL_S,
        .get_heartbeat_interval_s = tele_presence_get_heartbeat_interval_s,
        .apply_heartbeat_interval_s = tele_presence_apply_heartbeat_interval_s,
    };

    if (s_started) {
        return ESP_OK;
    }

    if (CONFIG_MQTT_BROKER_URI[0] == '\0') {
        ESP_LOGW(TAG, "MQTT habilitado sem broker URI; modulo nao iniciado");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = tele_system_registry_register(&registry_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar registry de sistema: %s", esp_err_to_name(err));
        return err;
    }

    mqtt_config.heartbeat_interval_s =
        tele_system_registry_get_effective_heartbeat_interval_s(CONFIG_MQTT_HEARTBEAT_INTERVAL_S);

    if (!s_wifi_event_registered) {
        err = esp_event_handler_register(WIFI_MANAGER_EVENT,
                                         ESP_EVENT_ANY_ID,
                                         tele_presence_wifi_event_handler,
                                         NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao registrar eventos Wi-Fi para MQTT: %s", esp_err_to_name(err));
            return err;
        }
        s_wifi_event_registered = true;
    }

    err = tele_mqtt_start(&mqtt_config);
    if (err != ESP_OK) {
        return err;
    }

    s_started = true;
    return ESP_OK;
#endif
}
