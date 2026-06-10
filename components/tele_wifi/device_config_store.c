#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "device_config_store.h"

#define DEVICE_CONFIG_NVS_NAMESPACE "provisioning"
#define DEVICE_CONFIG_NVS_SSID_KEY "ssid"
#define DEVICE_CONFIG_NVS_STA_RETRY_KEY "sta_retry"
#define DEVICE_CONFIG_NVS_APSTA_POLICY_KEY "apsta_policy"
#define DEVICE_CONFIG_NVS_APSTA_GRACE_KEY "apsta_grace"

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

static const char *TAG = "device-config";

static bool apsta_policy_valid(device_config_apsta_policy_t policy)
{
    return policy == DEVICE_CONFIG_APSTA_ALWAYS_ON ||
           policy == DEVICE_CONFIG_APSTA_AUTO_TIMEOUT ||
           policy == DEVICE_CONFIG_APSTA_STA_ONLY;
}

static bool apsta_grace_valid(uint32_t grace_period_s)
{
    return grace_period_s >= DEVICE_CONFIG_APSTA_GRACE_PERIOD_S_MIN &&
           grace_period_s <= DEVICE_CONFIG_APSTA_GRACE_PERIOD_S_MAX;
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
    nvs_handle_t handle = 0;
    size_t required_size = out_size;
    esp_err_t err = ESP_OK;

    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, out_size);

    err = nvs_open(DEVICE_CONFIG_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, DEVICE_CONFIG_NVS_SSID_KEY, out, &required_size);
        nvs_close(handle);

        if (err == ESP_OK) {
            trim_trailing_whitespace(out);
            if (out[0] != '\0') {
                ESP_LOGI(TAG, "SSID de provisionamento carregado da NVS");
                return ESP_OK;
            }
            ESP_LOGW(TAG, "SSID de provisionamento na NVS esta vazio");
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Falha ao ler SSID de provisionamento da NVS: %s", esp_err_to_name(err));
            return err;
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Falha ao abrir namespace de provisionamento na NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = copy_valid_ssid(out, out_size, CONFIG_WIFI_PROVISIONING_SSID);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SSID de provisionamento carregado do menuconfig");
        esp_err_t save_err = device_config_store_save_provisioning_ssid(out);
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "Nao foi possivel persistir SSID de provisionamento na NVS: %s",
                     esp_err_to_name(save_err));
        }
    }
    return err;
}

esp_err_t device_config_store_save_provisioning_ssid(const char *ssid)
{
    char sanitized[DEVICE_CONFIG_PROVISIONING_SSID_BUFFER_SIZE] = {0};
    nvs_handle_t handle = 0;
    esp_err_t err = copy_valid_ssid(sanitized, sizeof(sanitized), ssid);

    if (err != ESP_OK) {
        return err;
    }

    err = nvs_open(DEVICE_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, DEVICE_CONFIG_NVS_SSID_KEY, sanitized);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SSID de provisionamento salvo na NVS");
    }

    return err;
}

esp_err_t device_config_store_load_sta_max_retry(uint8_t *out_retry)
{
    nvs_handle_t handle = 0;
    uint8_t retry = 0;
    esp_err_t err = ESP_OK;

    if (!out_retry) {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(DEVICE_CONFIG_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_u8(handle, DEVICE_CONFIG_NVS_STA_RETRY_KEY, &retry);
        nvs_close(handle);

        if (err == ESP_OK) {
            if (retry < DEVICE_CONFIG_STA_MAX_RETRY_MIN || retry > DEVICE_CONFIG_STA_MAX_RETRY_MAX) {
                ESP_LOGW(TAG,
                         "Retry STA invalido na NVS (%u), usando menuconfig",
                         (unsigned)retry);
                err = ESP_ERR_INVALID_SIZE;
            } else {
                *out_retry = retry;
                ESP_LOGI(TAG, "Retry STA carregado da NVS: %u", (unsigned)retry);
                return ESP_OK;
            }
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Falha ao ler retry STA da NVS: %s", esp_err_to_name(err));
            return err;
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Falha ao abrir namespace de provisionamento na NVS: %s", esp_err_to_name(err));
        return err;
    }

    retry = (uint8_t)CONFIG_WIFI_STA_MAX_RETRY;
    if (retry < DEVICE_CONFIG_STA_MAX_RETRY_MIN || retry > DEVICE_CONFIG_STA_MAX_RETRY_MAX) {
        retry = 3;
    }

    *out_retry = retry;
    ESP_LOGI(TAG, "Retry STA carregado do menuconfig: %u", (unsigned)retry);
    err = device_config_store_save_sta_max_retry(retry);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Nao foi possivel persistir retry STA na NVS: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

esp_err_t device_config_store_save_sta_max_retry(uint8_t retry)
{
    nvs_handle_t handle = 0;
    esp_err_t err = ESP_OK;

    if (retry < DEVICE_CONFIG_STA_MAX_RETRY_MIN || retry > DEVICE_CONFIG_STA_MAX_RETRY_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(DEVICE_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, DEVICE_CONFIG_NVS_STA_RETRY_KEY, retry);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Retry STA salvo na NVS: %u", (unsigned)retry);
    }

    return err;
}

esp_err_t device_config_store_load_apsta_policy(device_config_apsta_policy_t *out_policy,
                                                uint32_t *out_grace_period_s)
{
    nvs_handle_t handle = 0;
    uint8_t policy = (uint8_t)DEVICE_CONFIG_APSTA_AUTO_TIMEOUT;
    uint32_t grace_period_s = CONFIG_WIFI_APSTA_GRACE_PERIOD_S;
    esp_err_t err = ESP_OK;

    if (!out_policy || !out_grace_period_s) {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(DEVICE_CONFIG_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_u8(handle, DEVICE_CONFIG_NVS_APSTA_POLICY_KEY, &policy);
        if (err == ESP_OK) {
            err = nvs_get_u32(handle, DEVICE_CONFIG_NVS_APSTA_GRACE_KEY, &grace_period_s);
        }
        nvs_close(handle);

        if (err == ESP_OK) {
            if (!apsta_policy_valid((device_config_apsta_policy_t)policy) ||
                !apsta_grace_valid(grace_period_s)) {
                ESP_LOGW(TAG,
                         "Politica APSTA invalida na NVS (policy=%u grace=%lu), usando defaults",
                         (unsigned)policy,
                         (unsigned long)grace_period_s);
                err = ESP_ERR_INVALID_SIZE;
            } else {
                *out_policy = (device_config_apsta_policy_t)policy;
                *out_grace_period_s = grace_period_s;
                ESP_LOGI(TAG,
                         "Politica APSTA carregada da NVS: policy=%u grace=%lu",
                         (unsigned)policy,
                         (unsigned long)grace_period_s);
                return ESP_OK;
            }
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Falha ao ler politica APSTA da NVS: %s", esp_err_to_name(err));
            return err;
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Falha ao abrir namespace de provisionamento na NVS: %s", esp_err_to_name(err));
        return err;
    }

    policy = (uint8_t)CONFIG_WIFI_APSTA_POLICY;
    grace_period_s = CONFIG_WIFI_APSTA_GRACE_PERIOD_S;
    if (!apsta_policy_valid((device_config_apsta_policy_t)policy)) {
        policy = (uint8_t)DEVICE_CONFIG_APSTA_AUTO_TIMEOUT;
    }
    if (!apsta_grace_valid(grace_period_s)) {
        grace_period_s = 600;
    }

    *out_policy = (device_config_apsta_policy_t)policy;
    *out_grace_period_s = grace_period_s;

    err = device_config_store_save_apsta_policy(*out_policy, *out_grace_period_s);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Nao foi possivel persistir politica APSTA na NVS: %s", esp_err_to_name(err));
    }

    return ESP_OK;
}

esp_err_t device_config_store_save_apsta_policy(device_config_apsta_policy_t policy,
                                                uint32_t grace_period_s)
{
    nvs_handle_t handle = 0;
    esp_err_t err = ESP_OK;

    if (!apsta_policy_valid(policy) || !apsta_grace_valid(grace_period_s)) {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(DEVICE_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, DEVICE_CONFIG_NVS_APSTA_POLICY_KEY, (uint8_t)policy);
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, DEVICE_CONFIG_NVS_APSTA_GRACE_KEY, grace_period_s);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "Politica APSTA salva na NVS: policy=%u grace=%lu",
                 (unsigned)policy,
                 (unsigned long)grace_period_s);
    }

    return err;
}
