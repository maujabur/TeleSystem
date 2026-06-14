#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "wifi_config.h"

#define WIFI_NAMESPACE "wifi"
#define WIFI_KEY_SSID "ssid"
#define WIFI_KEY_PASSWORD "password"
#define WIFI_KEY_SAVED_NETWORKS "networks_v1"
#define WIFI_SAVED_NETWORKS_VERSION 1U

static const char *TAG = "wifi-config";

#if CONFIG_WIFI_MULTI_NETWORK_CREDENTIALS
typedef struct {
    char ssid[WIFI_CONFIG_SSID_MAX_LEN + 1];
    char password[WIFI_CONFIG_PASSWORD_MAX_LEN + 1];
    int32_t priority;
} wifi_saved_network_blob_entry_t;

typedef struct {
    uint32_t version;
    uint32_t count;
    wifi_saved_network_blob_entry_t entries[WIFI_CONFIG_MAX_SAVED_NETWORKS];
} wifi_saved_networks_blob_t;
#endif

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

#if CONFIG_WIFI_MULTI_NETWORK_CREDENTIALS
static void normalize_network(wifi_saved_network_t *network)
{
    if (!network) {
        return;
    }

    trim_trailing_whitespace(network->ssid);
    trim_trailing_whitespace(network->password);
}

static void sort_networks_by_priority(wifi_saved_network_t *networks, size_t count)
{
    size_t i = 0;
    size_t j = 0;

    if (!networks || count < 2) {
        return;
    }

    for (i = 0; i + 1 < count; ++i) {
        for (j = i + 1; j < count; ++j) {
            if (networks[j].priority > networks[i].priority) {
                wifi_saved_network_t tmp = networks[i];
                networks[i] = networks[j];
                networks[j] = tmp;
            }
        }
    }
}

static esp_err_t load_blob(nvs_handle_t handle, wifi_saved_networks_blob_t *blob)
{
    size_t blob_size = sizeof(*blob);
    esp_err_t err = ESP_OK;

    if (!blob) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(blob, 0, sizeof(*blob));
    err = nvs_get_blob(handle, WIFI_KEY_SAVED_NETWORKS, blob, &blob_size);
    if (err != ESP_OK) {
        return err;
    }

    if (blob_size != sizeof(*blob) || blob->version != WIFI_SAVED_NETWORKS_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }

    if (blob->count > WIFI_CONFIG_MAX_SAVED_NETWORKS) {
        blob->count = WIFI_CONFIG_MAX_SAVED_NETWORKS;
    }

    return ESP_OK;
}

static esp_err_t save_blob(nvs_handle_t handle,
                           const wifi_saved_network_t *networks,
                           size_t network_count)
{
    wifi_saved_networks_blob_t *blob = NULL;
    size_t i = 0;
    esp_err_t err = ESP_OK;

    if ((!networks && network_count > 0) || network_count > WIFI_CONFIG_MAX_SAVED_NETWORKS) {
        return ESP_ERR_INVALID_ARG;
    }

    blob = (wifi_saved_networks_blob_t *)calloc(1, sizeof(*blob));
    if (!blob) {
        return ESP_ERR_NO_MEM;
    }

    blob->version = WIFI_SAVED_NETWORKS_VERSION;
    blob->count = (uint32_t)network_count;

    for (i = 0; i < network_count; ++i) {
        snprintf(blob->entries[i].ssid, sizeof(blob->entries[i].ssid), "%s", networks[i].ssid);
        snprintf(blob->entries[i].password, sizeof(blob->entries[i].password), "%s", networks[i].password);
        blob->entries[i].priority = networks[i].priority;
    }

    err = nvs_set_blob(handle, WIFI_KEY_SAVED_NETWORKS, blob, sizeof(*blob));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    free(blob);
    return err;
}

#endif

esp_err_t wifi_config_load(wifi_credentials_t *cfg)
{
#if CONFIG_WIFI_MULTI_NETWORK_CREDENTIALS
    wifi_saved_network_t *networks = NULL;
    size_t network_count = 0;
    esp_err_t err = ESP_OK;

    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(cfg, 0, sizeof(*cfg));

    networks = (wifi_saved_network_t *)calloc(WIFI_CONFIG_MAX_SAVED_NETWORKS, sizeof(*networks));
    if (!networks) {
        return ESP_ERR_NO_MEM;
    }

    err = wifi_config_load_saved_networks(networks,
                                          WIFI_CONFIG_MAX_SAVED_NETWORKS,
                                          &network_count);
    if (err != ESP_OK) {
        free(networks);
        return err;
    }

    if (network_count == 0) {
        free(networks);
        return ESP_ERR_NOT_FOUND;
    }

    snprintf(cfg->ssid, sizeof(cfg->ssid), "%s", networks[0].ssid);
    snprintf(cfg->password, sizeof(cfg->password), "%s", networks[0].password);
    cfg->provisioned = cfg->ssid[0] != '\0';
    free(networks);

    if (cfg->provisioned) {
        ESP_LOGI(TAG, "Credenciais Wi-Fi carregadas da NVS");
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
#else
    nvs_handle_t handle = 0;
    size_t ssid_len = 0;
    size_t password_len = 0;
    esp_err_t err = ESP_OK;

    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(cfg, 0, sizeof(*cfg));

    err = nvs_open(WIFI_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    ssid_len = sizeof(cfg->ssid);
    err = nvs_get_str(handle, WIFI_KEY_SSID, cfg->ssid, &ssid_len);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    password_len = sizeof(cfg->password);
    err = nvs_get_str(handle, WIFI_KEY_PASSWORD, cfg->password, &password_len);
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }

    trim_trailing_whitespace(cfg->ssid);
    trim_trailing_whitespace(cfg->password);
    cfg->provisioned = cfg->ssid[0] != '\0';

    if (cfg->provisioned) {
        ESP_LOGI(TAG, "Credenciais Wi-Fi carregadas da NVS");
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
#endif
}

esp_err_t wifi_config_save(const wifi_credentials_t *cfg)
{
#if CONFIG_WIFI_MULTI_NETWORK_CREDENTIALS
    wifi_saved_network_t network = {0};

    if (!cfg || cfg->ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(network.ssid, sizeof(network.ssid), "%s", cfg->ssid);
    snprintf(network.password, sizeof(network.password), "%s", cfg->password);
    network.priority = WIFI_CONFIG_PRIORITY_AUTO;

    return wifi_config_upsert_saved_network(&network);
#else
    nvs_handle_t handle = 0;
    esp_err_t err = ESP_OK;

    if (!cfg || cfg->ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, WIFI_KEY_SSID, cfg->ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, WIFI_KEY_PASSWORD, cfg->password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Credenciais Wi-Fi persistidas na NVS");
    }

    return err;
#endif
}

esp_err_t wifi_config_clear(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &handle);

    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_key(handle, WIFI_KEY_SSID);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        esp_err_t password_err = nvs_erase_key(handle, WIFI_KEY_PASSWORD);
        if (password_err != ESP_OK && password_err != ESP_ERR_NVS_NOT_FOUND) {
            err = password_err;
        }
    }
#if CONFIG_WIFI_MULTI_NETWORK_CREDENTIALS
    if (err == ESP_OK) {
        esp_err_t list_err = nvs_erase_key(handle, WIFI_KEY_SAVED_NETWORKS);
        if (list_err != ESP_OK && list_err != ESP_ERR_NVS_NOT_FOUND) {
            err = list_err;
        }
    }
#endif
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

esp_err_t wifi_config_load_saved_networks(wifi_saved_network_t *networks,
                                          size_t max_networks,
                                          size_t *network_count)
{
#if CONFIG_WIFI_MULTI_NETWORK_CREDENTIALS
    nvs_handle_t handle = 0;
    wifi_saved_networks_blob_t *blob = NULL;
    esp_err_t err = ESP_OK;
    size_t out_count = 0;
    size_t i = 0;

    if (!networks || !network_count || max_networks == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *network_count = 0;
    memset(networks, 0, max_networks * sizeof(*networks));

    blob = (wifi_saved_networks_blob_t *)calloc(1, sizeof(*blob));
    if (!blob) {
        return ESP_ERR_NO_MEM;
    }

    err = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        free(blob);
        return err;
    }

    err = load_blob(handle, blob);
    if (err == ESP_OK) {
        out_count = blob->count;
        if (out_count > max_networks) {
            out_count = max_networks;
        }

        for (i = 0; i < out_count; ++i) {
            snprintf(networks[i].ssid, sizeof(networks[i].ssid), "%s", blob->entries[i].ssid);
            snprintf(networks[i].password, sizeof(networks[i].password), "%s", blob->entries[i].password);
            networks[i].priority = blob->entries[i].priority;
            normalize_network(&networks[i]);
        }

        sort_networks_by_priority(networks, out_count);
        *network_count = out_count;
        ESP_LOGI(TAG, "Redes carregadas do NVS multi-rede: count=%zu", out_count);
        for (size_t debug_i = 0; debug_i < out_count; debug_i++) {
            ESP_LOGI(TAG, "  [%zu] ssid=%s priority=%ld", debug_i, networks[debug_i].ssid, (long)networks[debug_i].priority);
        }
        free(blob);
        nvs_close(handle);
        return out_count > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
    }

    free(blob);
    nvs_close(handle);
    return ESP_ERR_NOT_FOUND;
#else
    wifi_credentials_t single_network = {0};
    esp_err_t err = ESP_OK;

    if (!networks || !network_count || max_networks == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *network_count = 0;
    memset(networks, 0, max_networks * sizeof(*networks));

    err = wifi_config_load(&single_network);
    if (err != ESP_OK) {
        return err;
    }

    snprintf(networks[0].ssid, sizeof(networks[0].ssid), "%s", single_network.ssid);
    snprintf(networks[0].password, sizeof(networks[0].password), "%s", single_network.password);
    networks[0].priority = 0;
    *network_count = 1;
    return ESP_OK;
#endif
}

esp_err_t wifi_config_upsert_saved_network(const wifi_saved_network_t *network)
{
#if CONFIG_WIFI_MULTI_NETWORK_CREDENTIALS
    nvs_handle_t handle = 0;
    wifi_saved_network_t *saved = NULL;
    wifi_saved_network_t incoming = {0};
    size_t saved_count = 0;
    size_t i = 0;
    int existing_idx = -1;
    int replace_idx = -1;
    int32_t max_priority = 0;
    esp_err_t err = ESP_OK;

    if (!network || network->ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    incoming = *network;
    normalize_network(&incoming);
    if (incoming.ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    saved = (wifi_saved_network_t *)calloc(WIFI_CONFIG_MAX_SAVED_NETWORKS, sizeof(*saved));
    if (!saved) {
        return ESP_ERR_NO_MEM;
    }

    err = wifi_config_load_saved_networks(saved,
                                          WIFI_CONFIG_MAX_SAVED_NETWORKS,
                                          &saved_count);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        free(saved);
        return err;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        saved_count = 0;
    }

    for (i = 0; i < saved_count; ++i) {
        if (strcmp(saved[i].ssid, incoming.ssid) == 0) {
            existing_idx = (int)i;
        }
        if (saved[i].priority > max_priority || i == 0) {
            max_priority = saved[i].priority;
        }
    }

    if (incoming.priority == WIFI_CONFIG_PRIORITY_AUTO) {
        incoming.priority = saved_count == 0 ? 0 : (max_priority + 1);
    }

    if (existing_idx >= 0) {
        saved[existing_idx] = incoming;
    } else if (saved_count < WIFI_CONFIG_MAX_SAVED_NETWORKS) {
        saved[saved_count++] = incoming;
    } else {
        replace_idx = 0;
        for (i = 1; i < saved_count; ++i) {
            if (saved[i].priority < saved[replace_idx].priority) {
                replace_idx = (int)i;
            }
        }
        saved[replace_idx] = incoming;
    }

    sort_networks_by_priority(saved, saved_count);

    err = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        free(saved);
        return err;
    }

    err = save_blob(handle, saved, saved_count);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, WIFI_KEY_SSID, saved[0].ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, WIFI_KEY_PASSWORD, saved[0].password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    } else {
        ESP_LOGE(TAG, "Erro ao salvar redes: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    free(saved);
    return err;
#else
    wifi_credentials_t cfg = {0};

    if (!network || network->ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(cfg.ssid, sizeof(cfg.ssid), "%s", network->ssid);
    snprintf(cfg.password, sizeof(cfg.password), "%s", network->password);
    cfg.provisioned = true;
    return wifi_config_save(&cfg);
#endif
}

esp_err_t wifi_config_remove_saved_network(const char *ssid)
{
#if CONFIG_WIFI_MULTI_NETWORK_CREDENTIALS
    nvs_handle_t handle = 0;
    wifi_saved_network_t *saved = NULL;
    size_t saved_count = 0;
    size_t i = 0;
    bool removed = false;
    esp_err_t err = ESP_OK;

    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    saved = (wifi_saved_network_t *)calloc(WIFI_CONFIG_MAX_SAVED_NETWORKS, sizeof(*saved));
    if (!saved) {
        return ESP_ERR_NO_MEM;
    }

    err = wifi_config_load_saved_networks(saved,
                                          WIFI_CONFIG_MAX_SAVED_NETWORKS,
                                          &saved_count);
    if (err != ESP_OK) {
        free(saved);
        return err;
    }

    for (i = 0; i < saved_count; ++i) {
        if (strcmp(saved[i].ssid, ssid) == 0) {
            size_t j = 0;
            for (j = i; j + 1 < saved_count; ++j) {
                saved[j] = saved[j + 1];
            }
            memset(&saved[saved_count - 1], 0, sizeof(saved[saved_count - 1]));
            saved_count--;
            removed = true;
            break;
        }
    }

    if (!removed) {
        free(saved);
        return ESP_ERR_NOT_FOUND;
    }

    err = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        free(saved);
        return err;
    }

    if (saved_count == 0) {
        err = nvs_erase_key(handle, WIFI_KEY_SAVED_NETWORKS);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        if (err == ESP_OK) {
            err = nvs_erase_key(handle, WIFI_KEY_SSID);
            if (err == ESP_ERR_NVS_NOT_FOUND) {
                err = ESP_OK;
            }
        }
        if (err == ESP_OK) {
            err = nvs_erase_key(handle, WIFI_KEY_PASSWORD);
            if (err == ESP_ERR_NVS_NOT_FOUND) {
                err = ESP_OK;
            }
        }
    } else {
        sort_networks_by_priority(saved, saved_count);
        err = save_blob(handle, saved, saved_count);
        if (err == ESP_OK) {
            err = nvs_set_str(handle, WIFI_KEY_SSID, saved[0].ssid);
        }
        if (err == ESP_OK) {
            err = nvs_set_str(handle, WIFI_KEY_PASSWORD, saved[0].password);
        }
    }

    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    free(saved);
    return err;
#else
    wifi_credentials_t cfg = {0};
    esp_err_t err = wifi_config_load(&cfg);

    if (err != ESP_OK) {
        return err;
    }
    if (strcmp(cfg.ssid, ssid) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    return wifi_config_clear();
#endif
}
