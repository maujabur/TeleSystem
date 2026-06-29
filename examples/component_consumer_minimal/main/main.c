#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "tele_channels.h"
#include "tele_commands.h"
#include "tele_config.h"
#include "tele_status.h"

static const char *TAG = "consumer-min";

static uint32_t read_uptime_s(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_log_timestamp() / 1000U);
}

static const tele_config_field_t s_config_fields[] = {
    {
        .id = "example.sample_interval_s",
        .nvs_key = "ex_sample",
        .type = TELE_CONFIG_TYPE_U32,
        .default_value.u32 = 60,
        .min.u32 = 5,
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
        .description = "Comando demonstrativo registrado sem MQTT.",
        .group = "example",
        .channel_flags = TELE_CHANNEL_FLAG_MQTT,
    },
};

static void log_json(const char *label, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    if (!text) {
        ESP_LOGE(TAG, "Falha ao serializar %s", label);
        return;
    }
    ESP_LOGI(TAG, "%s: %s", label, text);
    cJSON_free(text);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(tele_config_register_fields(
        s_config_fields,
        sizeof(s_config_fields) / sizeof(s_config_fields[0])));
    ESP_ERROR_CHECK(tele_status_register_fields(
        s_status_fields,
        sizeof(s_status_fields) / sizeof(s_status_fields[0])));
    ESP_ERROR_CHECK(tele_commands_register(
        s_commands,
        sizeof(s_commands) / sizeof(s_commands[0])));

    cJSON *config_manifest = cJSON_CreateObject();
    cJSON *status_manifest = cJSON_CreateObject();
    cJSON *commands_manifest = cJSON_CreateObject();
    cJSON *state = cJSON_CreateObject();
    if (!config_manifest || !status_manifest || !commands_manifest || !state) {
        abort();
    }

    ESP_ERROR_CHECK(tele_config_add_manifest_to_json(config_manifest, TELE_CHANNEL_FLAG_MQTT));
    ESP_ERROR_CHECK(tele_status_add_manifest_to_json(status_manifest, TELE_CHANNEL_FLAG_MQTT));
    ESP_ERROR_CHECK(tele_commands_add_manifest_to_json(commands_manifest, TELE_CHANNEL_FLAG_MQTT));
    ESP_ERROR_CHECK(tele_status_add_fields_to_json(state, TELE_CHANNEL_FLAG_MQTT, TELE_STATUS_FLAG_STATE));

    log_json("meta/config", config_manifest);
    log_json("meta/status", status_manifest);
    log_json("meta/commands", commands_manifest);
    log_json("state", state);

    cJSON_Delete(config_manifest);
    cJSON_Delete(status_manifest);
    cJSON_Delete(commands_manifest);
    cJSON_Delete(state);
}
