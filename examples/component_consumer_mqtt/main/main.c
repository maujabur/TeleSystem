#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "tele_channels.h"
#include "tele_commands.h"
#include "tele_config.h"
#include "tele_mqtt.h"
#include "tele_status.h"

#ifndef CONFIG_EXAMPLE_MQTT_ENABLED
#define CONFIG_EXAMPLE_MQTT_ENABLED 0
#endif

#ifndef CONFIG_EXAMPLE_MQTT_BROKER_URI
#define CONFIG_EXAMPLE_MQTT_BROKER_URI ""
#endif

#ifndef CONFIG_EXAMPLE_MQTT_USERNAME
#define CONFIG_EXAMPLE_MQTT_USERNAME ""
#endif

#ifndef CONFIG_EXAMPLE_MQTT_PASSWORD
#define CONFIG_EXAMPLE_MQTT_PASSWORD ""
#endif

#ifndef CONFIG_EXAMPLE_MQTT_BASE_TOPIC
#define CONFIG_EXAMPLE_MQTT_BASE_TOPIC "v1/example"
#endif

#ifndef CONFIG_EXAMPLE_MQTT_DEVICE_ID_PREFIX
#define CONFIG_EXAMPLE_MQTT_DEVICE_ID_PREFIX "ExampleDevice"
#endif

#define EXAMPLE_FW_VERSION "0.1.0 component consumer mqtt"

static const char *TAG = "consumer-mqtt";

static uint32_t read_uptime_s(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_log_timestamp() / 1000U);
}

static bool mqtt_ready(void *ctx)
{
    (void)ctx;
    return true;
}

static bool build_timestamp(char *buffer, size_t buffer_len, void *ctx)
{
    (void)ctx;
    if (!buffer || buffer_len == 0) {
        return false;
    }
    snprintf(buffer, buffer_len, "1970-01-01T00:00:00Z");
    return true;
}

static const tele_config_field_t s_config_fields[] = {
    {
        .id = "example.heartbeat_interval_s",
        .nvs_key = "ex_hb",
        .type = TELE_CONFIG_TYPE_U32,
        .default_value.u32 = 60,
        .min.u32 = 15,
        .max.u32 = 3600,
        .channel_flags = TELE_CHANNEL_FLAG_MQTT | TELE_CHANNEL_FLAG_WEB,
    },
};

static const tele_status_field_t s_status_fields[] = {
    {
        .id = "uptime_s",
        .label = "Uptime",
        .description = "Tempo desde o boot.",
        .group = "runtime",
        .type = TELE_STATUS_TYPE_U32,
        .unit = "s",
        .channel_flags = TELE_CHANNEL_FLAG_MQTT,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_HEARTBEAT,
        .read.u32 = read_uptime_s,
    },
};

static const tele_command_t s_commands[] = {
    {
        .name = "example/ping",
        .label = "Ping exemplo",
        .description = "Comando de produto demonstrativo.",
        .group = "example",
        .channel_flags = TELE_CHANNEL_FLAG_MQTT,
    },
};

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(tele_config_register_fields(
        s_config_fields,
        sizeof(s_config_fields) / sizeof(s_config_fields[0])));
    ESP_ERROR_CHECK(tele_status_register_fields(
        s_status_fields,
        sizeof(s_status_fields) / sizeof(s_status_fields[0])));
    ESP_ERROR_CHECK(tele_commands_register(
        s_commands,
        sizeof(s_commands) / sizeof(s_commands[0])));

    if (!CONFIG_EXAMPLE_MQTT_ENABLED) {
        ESP_LOGI(TAG, "MQTT desativado no exemplo; registries inicializados");
        return;
    }

    if (CONFIG_EXAMPLE_MQTT_BROKER_URI[0] == '\0') {
        ESP_LOGW(TAG, "MQTT habilitado sem broker URI; cliente nao iniciado");
        return;
    }

    tele_mqtt_config_t mqtt_config = {
        .broker_uri = CONFIG_EXAMPLE_MQTT_BROKER_URI,
        .username = CONFIG_EXAMPLE_MQTT_USERNAME,
        .password = CONFIG_EXAMPLE_MQTT_PASSWORD,
        .base_topic = CONFIG_EXAMPLE_MQTT_BASE_TOPIC,
        .device_id_prefix = CONFIG_EXAMPLE_MQTT_DEVICE_ID_PREFIX,
        .firmware_version = EXAMPLE_FW_VERSION,
        .heartbeat_interval_s = 60,
        .keepalive_s = 60,
        .qos_critical = 1,
        .qos_telemetry = 0,
        .is_ready = mqtt_ready,
        .build_timestamp = build_timestamp,
    };

    ESP_ERROR_CHECK(tele_mqtt_start(&mqtt_config));
}
