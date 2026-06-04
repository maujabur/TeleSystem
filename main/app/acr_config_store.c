#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "acr_config_store.h"

#define ACR_NVS_NAMESPACE "acr"
#define ACR_NVS_REGION_KEY "region"
#define ACR_NVS_CONTAINER_ID_KEY "container_id"
#define ACR_NVS_TOKEN_KEY "bearer_token"
#define ACR_NVS_UPLOAD_PREFIX_KEY "upload_prefix"

#ifndef CONFIG_ACR_UPLOAD_PREFIX
#define CONFIG_ACR_UPLOAD_PREFIX "skips_999"
#endif

static const char *TAG = "acr-config";

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

static int is_upload_prefix_char_allowed(char c)
{
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '_' ||
           c == '-';
}

static void sanitize_upload_prefix(char *prefix)
{
    size_t read_index = 0;
    size_t write_index = 0;

    if (!prefix) {
        return;
    }

    while (prefix[read_index] != '\0') {
        char c = prefix[read_index++];

        if (is_upload_prefix_char_allowed(c)) {
            prefix[write_index++] = c;
        } else if (write_index > 0 && prefix[write_index - 1] != '_') {
            prefix[write_index++] = '_';
        }
    }

    while (write_index > 0 && prefix[write_index - 1] == '_') {
        write_index--;
    }
    prefix[write_index] = '\0';
}

static esp_err_t load_string_from_menuconfig(char *out,
                                             size_t out_size,
                                             const char *fallback_value,
                                             const char *label)
{
    memset(out, 0, out_size);

    if (!fallback_value || fallback_value[0] == '\0') {
        ESP_LOGE(TAG, "%s nao configurado na NVS nem no menuconfig", label);
        return ESP_ERR_NOT_FOUND;
    }

    if (snprintf(out, out_size, "%s", fallback_value) >= (int)out_size) {
        ESP_LOGE(TAG, "%s do menuconfig grande demais", label);
        return ESP_FAIL;
    }

    trim_trailing_whitespace(out);
    if (out[0] == '\0') {
        ESP_LOGE(TAG, "%s do menuconfig esta vazio", label);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "%s carregado do menuconfig", label);
    return ESP_OK;
}

static esp_err_t load_string_from_nvs(const char *key, char *out, size_t out_size, const char *label)
{
    nvs_handle_t handle = 0;
    size_t required_size = out_size;
    esp_err_t err = ESP_OK;

    if (!key || !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, out_size);

    err = nvs_open(ACR_NVS_NAMESPACE, NVS_READONLY, &handle);

    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_str(handle, key, out, &required_size);
    nvs_close(handle);

    if (err != ESP_OK) {
        return err;
    }

    trim_trailing_whitespace(out);
    if (out[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "%s carregado da NVS", label);
    return ESP_OK;
}

static esp_err_t save_string_to_nvs(const char *key, const char *value, const char *label)
{
    nvs_handle_t handle = 0;
    esp_err_t err = ESP_OK;

    if (!key || !value || value[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(ACR_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s salvo na NVS", label);
    }

    return err;
}

static esp_err_t save_sanitized_upload_prefix_to_nvs(const char *prefix)
{
    char sanitized[ACR_UPLOAD_PREFIX_BUFFER_SIZE] = {0};

    if (!prefix || prefix[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (snprintf(sanitized, sizeof(sanitized), "%s", prefix) >= (int)sizeof(sanitized)) {
        return ESP_ERR_INVALID_SIZE;
    }

    trim_trailing_whitespace(sanitized);
    sanitize_upload_prefix(sanitized);
    if (sanitized[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    return save_string_to_nvs(ACR_NVS_UPLOAD_PREFIX_KEY, sanitized, "Prefixo de upload ACR");
}

static esp_err_t load_config_string(const char *key,
                                    char *out,
                                    size_t out_size,
                                    const char *fallback_value,
                                    const char *label)
{
    esp_err_t err = load_string_from_nvs(key, out, out_size, label);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    if (err != ESP_ERR_NVS_NOT_FOUND && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "Falha ao ler %s da NVS: %s", label, esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "%s ainda nao provisionado na NVS", label);
    err = load_string_from_menuconfig(out, out_size, fallback_value, label);
    if (err != ESP_OK) {
        return err;
    }

    esp_err_t save_err = save_string_to_nvs(key, out, label);
    if (save_err != ESP_OK) {
        ESP_LOGW(TAG, "Nao foi possivel persistir %s na NVS: %s",
                 label,
                 esp_err_to_name(save_err));
    }

    return ESP_OK;
}

esp_err_t acr_config_store_load(acr_config_t *config)
{
    esp_err_t err = ESP_OK;

    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));

    err = load_config_string(ACR_NVS_REGION_KEY,
                             config->region,
                             sizeof(config->region),
                             CONFIG_ACR_REGION,
                             "ACR region");
    if (err != ESP_OK) {
        return err;
    }

    err = load_config_string(ACR_NVS_CONTAINER_ID_KEY,
                             config->container_id,
                             sizeof(config->container_id),
                             CONFIG_ACR_CONTAINER_ID,
                             "ACR container ID");
    if (err != ESP_OK) {
        return err;
    }

    err = load_config_string(ACR_NVS_TOKEN_KEY,
                             config->bearer_token,
                             sizeof(config->bearer_token),
                             CONFIG_ACR_BEARER_TOKEN,
                             "Bearer token");
    if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

esp_err_t acr_config_store_get_public_info(acr_config_public_info_t *info)
{
    char token[ACR_TOKEN_BUFFER_SIZE] = {0};
    esp_err_t err = ESP_OK;

    if (!info) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(*info));

    err = load_config_string(ACR_NVS_REGION_KEY,
                             info->region,
                             sizeof(info->region),
                             CONFIG_ACR_REGION,
                             "ACR region");
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        return err;
    }

    err = load_config_string(ACR_NVS_CONTAINER_ID_KEY,
                             info->container_id,
                             sizeof(info->container_id),
                             CONFIG_ACR_CONTAINER_ID,
                             "ACR container ID");
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        return err;
    }

    err = acr_config_store_load_upload_prefix(info->upload_prefix, sizeof(info->upload_prefix));
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        return err;
    }

    err = load_config_string(ACR_NVS_TOKEN_KEY,
                             token,
                             sizeof(token),
                             CONFIG_ACR_BEARER_TOKEN,
                             "Bearer token");
    info->token_configured = err == ESP_OK && token[0] != '\0';

    return ESP_OK;
}

esp_err_t acr_config_store_load_upload_prefix(char *out, size_t out_size)
{
    esp_err_t err = load_config_string(ACR_NVS_UPLOAD_PREFIX_KEY,
                                       out,
                                       out_size,
                                       CONFIG_ACR_UPLOAD_PREFIX,
                                       "Prefixo de upload ACR");
    if (err != ESP_OK) {
        return err;
    }

    sanitize_upload_prefix(out);
    if (out[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

esp_err_t acr_config_store_save_region(const char *region)
{
    return save_string_to_nvs(ACR_NVS_REGION_KEY, region, "ACR region");
}

esp_err_t acr_config_store_save_container_id(const char *container_id)
{
    return save_string_to_nvs(ACR_NVS_CONTAINER_ID_KEY, container_id, "ACR container ID");
}

esp_err_t acr_config_store_save_bearer_token(const char *token)
{
    return save_string_to_nvs(ACR_NVS_TOKEN_KEY, token, "Bearer token");
}

esp_err_t acr_config_store_save_upload_prefix(const char *prefix)
{
    return save_sanitized_upload_prefix_to_nvs(prefix);
}
