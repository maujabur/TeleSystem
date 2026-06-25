#include "device_config.h"

#include <string.h>

#include "esp_log.h"

#include "tele_config.h"
#include "wifi_manager.h"

#ifndef CONFIG_WIFI_PROVISIONING_SSID
#define CONFIG_WIFI_PROVISIONING_SSID "ESP32-Device"
#endif

#ifndef CONFIG_WIFI_STA_MAX_RETRY
#define CONFIG_WIFI_STA_MAX_RETRY 3
#endif

#ifndef CONFIG_WIFI_APSTA_POLICY
#define CONFIG_WIFI_APSTA_POLICY 1
#endif

#ifndef CONFIG_WIFI_APSTA_GRACE_PERIOD_S
#define CONFIG_WIFI_APSTA_GRACE_PERIOD_S 600
#endif

static const char *TAG = "device-config";

static const tele_config_enum_choice_t s_apsta_policy_choices[] = {
    {.value = DEVICE_CONFIG_APSTA_ALWAYS_ON, .label = "always_on"},
    {.value = DEVICE_CONFIG_APSTA_AUTO_TIMEOUT, .label = "auto_timeout"},
    {.value = DEVICE_CONFIG_APSTA_STA_ONLY, .label = "sta_only"},
};

static const tele_config_field_t s_device_config_fields[] = {
    {
        .id = DEVICE_CONFIG_ID_PROVISIONING_SSID,
        .nvs_key = "w_pssid",
        .type = TELE_CONFIG_TYPE_STRING,
        .default_value.string = CONFIG_WIFI_PROVISIONING_SSID,
        .min_len = 1,
        .max_len = DEVICE_CONFIG_PROVISIONING_SSID_BUFFER_SIZE - 1,
        .flags = TELE_CONFIG_FLAG_WEB | TELE_CONFIG_FLAG_MQTT,
    },
    {
        .id = DEVICE_CONFIG_ID_STA_MAX_RETRY,
        .nvs_key = "w_retry",
        .type = TELE_CONFIG_TYPE_U32,
        .default_value.u32 = CONFIG_WIFI_STA_MAX_RETRY,
        .min.u32 = DEVICE_CONFIG_STA_MAX_RETRY_MIN,
        .max.u32 = DEVICE_CONFIG_STA_MAX_RETRY_MAX,
        .flags = TELE_CONFIG_FLAG_WEB | TELE_CONFIG_FLAG_MQTT,
    },
    {
        .id = DEVICE_CONFIG_ID_APSTA_POLICY,
        .nvs_key = "w_apsta",
        .type = TELE_CONFIG_TYPE_ENUM,
        .default_value.i32 = CONFIG_WIFI_APSTA_POLICY,
        .min.i32 = DEVICE_CONFIG_APSTA_ALWAYS_ON,
        .max.i32 = DEVICE_CONFIG_APSTA_STA_ONLY,
        .choices = s_apsta_policy_choices,
        .choice_count = sizeof(s_apsta_policy_choices) / sizeof(s_apsta_policy_choices[0]),
        .flags = TELE_CONFIG_FLAG_WEB | TELE_CONFIG_FLAG_MQTT | TELE_CONFIG_FLAG_REBOOT_REQUIRED,
    },
    {
        .id = DEVICE_CONFIG_ID_APSTA_GRACE_PERIOD_S,
        .nvs_key = "w_apgrace",
        .type = TELE_CONFIG_TYPE_U32,
        .default_value.u32 = CONFIG_WIFI_APSTA_GRACE_PERIOD_S,
        .min.u32 = DEVICE_CONFIG_APSTA_GRACE_PERIOD_S_MIN,
        .max.u32 = DEVICE_CONFIG_APSTA_GRACE_PERIOD_S_MAX,
        .flags = TELE_CONFIG_FLAG_WEB | TELE_CONFIG_FLAG_MQTT | TELE_CONFIG_FLAG_REBOOT_REQUIRED,
    },
};

static bool s_fields_registered;

static esp_err_t apply_device_config_field(const tele_config_field_t *field,
                                           const tele_config_value_t *value,
                                           void *ctx)
{
    (void)ctx;

    if (!field || !value) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(field->id, DEVICE_CONFIG_ID_PROVISIONING_SSID) == 0) {
        return wifi_manager_set_provisioning_ssid(value->string);
    }
    if (strcmp(field->id, DEVICE_CONFIG_ID_STA_MAX_RETRY) == 0) {
        return wifi_manager_set_sta_max_retry((int)value->u32);
    }
    if (strcmp(field->id, DEVICE_CONFIG_ID_APSTA_POLICY) == 0 ||
        strcmp(field->id, DEVICE_CONFIG_ID_APSTA_GRACE_PERIOD_S) == 0) {
        tele_config_value_t policy_value = {0};
        tele_config_value_t grace_value = {0};

        esp_err_t err = tele_config_get_effective(DEVICE_CONFIG_ID_APSTA_POLICY,
                                                  &policy_value,
                                                  NULL,
                                                  0,
                                                  NULL);
        if (err != ESP_OK) {
            return err;
        }

        err = tele_config_get_effective(DEVICE_CONFIG_ID_APSTA_GRACE_PERIOD_S,
                                        &grace_value,
                                        NULL,
                                        0,
                                        NULL);
        if (err != ESP_OK) {
            return err;
        }

        if (strcmp(field->id, DEVICE_CONFIG_ID_APSTA_POLICY) == 0) {
            policy_value.i32 = value->i32;
        } else {
            grace_value.u32 = value->u32;
        }

        return wifi_manager_set_apsta_policy((wifi_manager_apsta_policy_t)policy_value.i32,
                                             grace_value.u32);
    }

    return ESP_OK;
}

esp_err_t device_config_register_fields(void)
{
    if (s_fields_registered) {
        return ESP_OK;
    }

    esp_err_t err = tele_config_register_fields(s_device_config_fields,
                                                sizeof(s_device_config_fields) /
                                                    sizeof(s_device_config_fields[0]));
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        (void)tele_config_set_apply_handler(DEVICE_CONFIG_ID_PROVISIONING_SSID,
                                            apply_device_config_field,
                                            NULL);
        (void)tele_config_set_apply_handler(DEVICE_CONFIG_ID_STA_MAX_RETRY,
                                            apply_device_config_field,
                                            NULL);
        (void)tele_config_set_apply_handler(DEVICE_CONFIG_ID_APSTA_POLICY,
                                            apply_device_config_field,
                                            NULL);
        (void)tele_config_set_apply_handler(DEVICE_CONFIG_ID_APSTA_GRACE_PERIOD_S,
                                            apply_device_config_field,
                                            NULL);
        s_fields_registered = true;
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Falha ao registrar campos de configuracao: %s", esp_err_to_name(err));
    return err;
}
