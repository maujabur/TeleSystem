#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "device_config_store.h"
#include "tele_config.h"

#ifndef CONFIG_WIFI_PROVISIONING_SSID
#define CONFIG_WIFI_PROVISIONING_SSID "AiSkipsAi-999"
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

#define DEVICE_CONFIG_ID_PROVISIONING_SSID "wifi.provisioning_ssid"
#define DEVICE_CONFIG_ID_STA_MAX_RETRY "wifi.sta_max_retry"
#define DEVICE_CONFIG_ID_APSTA_POLICY "wifi.apsta_policy"
#define DEVICE_CONFIG_ID_APSTA_GRACE_PERIOD_S "wifi.apsta_grace_period_s"

static const char *TAG = "device-config";

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

static bool apsta_policy_valid(device_config_apsta_policy_t policy)
{
    return policy == DEVICE_CONFIG_APSTA_ALWAYS_ON ||
           policy == DEVICE_CONFIG_APSTA_AUTO_TIMEOUT ||
           policy == DEVICE_CONFIG_APSTA_STA_ONLY;
}

static esp_err_t ensure_fields_registered(void)
{
    if (s_fields_registered) {
        return ESP_OK;
    }

    esp_err_t err = tele_config_register_fields(s_device_config_fields,
                                                sizeof(s_device_config_fields) /
                                                    sizeof(s_device_config_fields[0]));
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        s_fields_registered = true;
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Falha ao registrar campos de configuracao: %s", esp_err_to_name(err));
    return err;
}

static void trim_trailing_whitespace(char *text)
{
    size_t len = 0;

    if (!text) {
        return;
    }

    len = strlen(text);
    while (len > 0) {
        char c = text[len - 1];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            break;
        }
        text[len - 1] = '\0';
        len--;
    }
}

static esp_err_t copy_valid_ssid(char *out, size_t out_size, const char *ssid)
{
    if (!out || out_size == 0 || !ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (snprintf(out, out_size, "%s", ssid) >= (int)out_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    trim_trailing_whitespace(out);
    if (out[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t device_config_store_load_provisioning_ssid(char *out, size_t out_size)
{
    tele_config_value_t value = {0};
    bool from_nvs = false;
    esp_err_t err = ESP_OK;

    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, out_size);

    err = ensure_fields_registered();
    if (err != ESP_OK) {
        return err;
    }

    err = tele_config_get_effective(DEVICE_CONFIG_ID_PROVISIONING_SSID,
                                   &value,
                                   out,
                                   out_size,
                                   &from_nvs);
    if (err != ESP_OK) {
        return err;
    }

    trim_trailing_whitespace(out);
    if (out[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG,
             "SSID de provisionamento carregado de %s",
             from_nvs ? "override NVS" : "menuconfig");
    return ESP_OK;
}

esp_err_t device_config_store_save_provisioning_ssid(const char *ssid)
{
    char sanitized[DEVICE_CONFIG_PROVISIONING_SSID_BUFFER_SIZE] = {0};
    tele_config_value_t value = {0};
    esp_err_t err = copy_valid_ssid(sanitized, sizeof(sanitized), ssid);

    if (err != ESP_OK) {
        return err;
    }

    err = ensure_fields_registered();
    if (err != ESP_OK) {
        return err;
    }

    value.string = sanitized;
    err = tele_config_set_override(DEVICE_CONFIG_ID_PROVISIONING_SSID, &value);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SSID de provisionamento salvo como override NVS");
    }
    return err;
}

esp_err_t device_config_store_load_sta_max_retry(uint8_t *out_retry)
{
    tele_config_value_t value = {0};
    bool from_nvs = false;
    esp_err_t err = ESP_OK;

    if (!out_retry) {
        return ESP_ERR_INVALID_ARG;
    }

    err = ensure_fields_registered();
    if (err != ESP_OK) {
        return err;
    }

    err = tele_config_get_effective(DEVICE_CONFIG_ID_STA_MAX_RETRY,
                                   &value,
                                   NULL,
                                   0,
                                   &from_nvs);
    if (err != ESP_OK) {
        return err;
    }

    *out_retry = (uint8_t)value.u32;
    ESP_LOGI(TAG,
             "Retry STA carregado de %s: %u",
             from_nvs ? "override NVS" : "menuconfig",
             (unsigned)*out_retry);
    return ESP_OK;
}

esp_err_t device_config_store_save_sta_max_retry(uint8_t retry)
{
    tele_config_value_t value = {.u32 = retry};
    esp_err_t err = ensure_fields_registered();

    if (err != ESP_OK) {
        return err;
    }

    err = tele_config_set_override(DEVICE_CONFIG_ID_STA_MAX_RETRY, &value);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Retry STA salvo como override NVS: %u", (unsigned)retry);
    }
    return err;
}

esp_err_t device_config_store_load_apsta_policy(device_config_apsta_policy_t *out_policy,
                                                uint32_t *out_grace_period_s)
{
    tele_config_value_t policy_value = {0};
    tele_config_value_t grace_value = {0};
    bool policy_from_nvs = false;
    bool grace_from_nvs = false;
    esp_err_t err = ESP_OK;

    if (!out_policy || !out_grace_period_s) {
        return ESP_ERR_INVALID_ARG;
    }

    err = ensure_fields_registered();
    if (err != ESP_OK) {
        return err;
    }

    err = tele_config_get_effective(DEVICE_CONFIG_ID_APSTA_POLICY,
                                   &policy_value,
                                   NULL,
                                   0,
                                   &policy_from_nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = tele_config_get_effective(DEVICE_CONFIG_ID_APSTA_GRACE_PERIOD_S,
                                   &grace_value,
                                   NULL,
                                   0,
                                   &grace_from_nvs);
    if (err != ESP_OK) {
        return err;
    }

    *out_policy = (device_config_apsta_policy_t)policy_value.i32;
    *out_grace_period_s = grace_value.u32;
    ESP_LOGI(TAG,
             "Politica APSTA carregada de %s/%s: policy=%u grace=%lu",
             policy_from_nvs ? "override NVS" : "menuconfig",
             grace_from_nvs ? "override NVS" : "menuconfig",
             (unsigned)*out_policy,
             (unsigned long)*out_grace_period_s);
    return ESP_OK;
}

esp_err_t device_config_store_save_apsta_policy(device_config_apsta_policy_t policy,
                                                uint32_t grace_period_s)
{
    tele_config_value_t policy_value = {.i32 = policy};
    tele_config_value_t grace_value = {.u32 = grace_period_s};
    esp_err_t err = ESP_OK;

    if (!apsta_policy_valid(policy)) {
        return ESP_ERR_INVALID_ARG;
    }

    err = ensure_fields_registered();
    if (err != ESP_OK) {
        return err;
    }

    err = tele_config_set_override(DEVICE_CONFIG_ID_APSTA_POLICY, &policy_value);
    if (err != ESP_OK) {
        return err;
    }

    err = tele_config_set_override(DEVICE_CONFIG_ID_APSTA_GRACE_PERIOD_S, &grace_value);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "Politica APSTA salva como override NVS: policy=%u grace=%lu",
                 (unsigned)policy,
                 (unsigned long)grace_period_s);
    }
    return err;
}
