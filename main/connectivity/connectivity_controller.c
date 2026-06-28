#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"

#include "app_indicators.h"
#include "boot_config_button.h"
#include "connectivity_controller.h"
#include "device_config.h"
#include "tele_config.h"
#include "tele_indicator.h"
#include "time_sync.h"
#include "web_portal.h"
#include "wifi_manager.h"

static const char *TAG = "connectivity";

static bool s_started;
static bool s_has_synced_wifi_state;
static wifi_manager_state_t s_last_synced_wifi_state;

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

static esp_err_t load_string_config(const char *id, char *out, size_t out_size)
{
    tele_config_value_t value = {0};

    if (!id || !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, out_size);
    esp_err_t err = tele_config_get_effective(id, &value, out, out_size, NULL);
    if (err != ESP_OK) {
        return err;
    }

    trim_trailing_whitespace(out);
    return out[0] == '\0' ? ESP_ERR_INVALID_ARG : ESP_OK;
}

static esp_err_t load_u32_config(const char *id, uint32_t *out_value)
{
    tele_config_value_t value = {0};

    if (!id || !out_value) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = tele_config_get_effective(id, &value, NULL, 0, NULL);
    if (err == ESP_OK) {
        *out_value = value.u32;
    }
    return err;
}

static esp_err_t load_i32_config(const char *id, int32_t *out_value)
{
    tele_config_value_t value = {0};

    if (!id || !out_value) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = tele_config_get_effective(id, &value, NULL, 0, NULL);
    if (err == ESP_OK) {
        *out_value = value.i32;
    }
    return err;
}

static void sync_web_portal_with_wifi_state(const wifi_manager_status_t *status)
{
    esp_err_t err = ESP_OK;

    if (status->state == WIFI_MANAGER_STATE_PROVISIONING_AP) {
        err = web_portal_start(true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Nao foi possivel iniciar portal captive: %s", esp_err_to_name(err));
        }
        return;
    }

    if (status->state == WIFI_MANAGER_STATE_STA_CONNECTED) {
        err = web_portal_start(false);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Nao foi possivel iniciar portal em modo normal: %s", esp_err_to_name(err));
        }
    }
}

static void sync_status_led_with_wifi_state(const wifi_manager_status_t *status)
{
    esp_err_t err = ESP_OK;
    const char *event_id = NULL;

    switch (status->state) {
    case WIFI_MANAGER_STATE_STA_CONNECTING:
        event_id = "wifi.connecting";
        break;
    case WIFI_MANAGER_STATE_STA_CONNECTED:
        event_id = "wifi.connected";
        break;
    case WIFI_MANAGER_STATE_PROVISIONING_AP:
        event_id = "wifi.provisioning";
        break;
    case WIFI_MANAGER_STATE_INIT:
    default:
        err = tele_indicator_clear_source("wifi");
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Nao foi possivel limpar indicador Wi-Fi: %s", esp_err_to_name(err));
        }
        return;
    }

    err = tele_indicator_clear_event("system.error");
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "Nao foi possivel limpar indicador de erro: %s", esp_err_to_name(err));
    }

    err = tele_indicator_raise("wifi", event_id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Nao foi possivel atualizar indicador Wi-Fi: %s", esp_err_to_name(err));
    }
}

static void sync_connectivity_outputs(bool force)
{
    wifi_manager_status_t status = {0};
    esp_err_t err = wifi_manager_get_status(&status);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Nao foi possivel consultar estado Wi-Fi: %s", esp_err_to_name(err));
        (void)tele_indicator_raise("system", "system.error");
        return;
    }

    if (!force && s_has_synced_wifi_state && status.state == s_last_synced_wifi_state) {
        ESP_LOGD(TAG,
                 "Sync de conectividade ignorado (estado Wi-Fi inalterado: %d)",
                 (int)status.state);
        return;
    }

    sync_web_portal_with_wifi_state(&status);
    sync_status_led_with_wifi_state(&status);
    s_last_synced_wifi_state = status.state;
    s_has_synced_wifi_state = true;
}

static void wifi_manager_event_handler(void *arg,
                                       esp_event_base_t event_base,
                                       int32_t event_id,
                                       void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    if (event_id == WIFI_MANAGER_EVENT_STA_CONNECTED) {
        time_sync_on_network_connected();
    } else if (event_id == WIFI_MANAGER_EVENT_STA_DISCONNECTED ||
               event_id == WIFI_MANAGER_EVENT_PROVISIONING_STARTED) {
        time_sync_on_network_disconnected();
    }

    if (event_id == WIFI_MANAGER_EVENT_PROVISIONING_STARTED ||
        event_id == WIFI_MANAGER_EVENT_STA_CONNECTED ||
        event_id == WIFI_MANAGER_EVENT_STA_DISCONNECTED ||
        event_id == WIFI_MANAGER_EVENT_STA_CONNECTING ||
        event_id == WIFI_MANAGER_EVENT_CREDENTIALS_UPDATED) {
        sync_connectivity_outputs(false);
    }
}

esp_err_t connectivity_controller_start(void)
{
    char provisioning_ssid[DEVICE_CONFIG_PROVISIONING_SSID_BUFFER_SIZE] = {0};
    uint32_t sta_max_retry = 0;
    int32_t apsta_policy = DEVICE_CONFIG_APSTA_AUTO_TIMEOUT;
    uint32_t apsta_grace_period_s = 600;
    wifi_manager_config_t wifi_config = {0};
    esp_err_t err = ESP_OK;

    if (s_started) {
        return ESP_OK;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_handler_register(WIFI_MANAGER_EVENT,
                                     ESP_EVENT_ANY_ID,
                                     &wifi_manager_event_handler,
                                     NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = app_indicators_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Indicador de status indisponivel: %s", esp_err_to_name(err));
    }

    err = time_sync_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Sincronizacao NTP indisponivel: %s", esp_err_to_name(err));
    }

    err = device_config_register_fields();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar configuracoes de dispositivo: %s", esp_err_to_name(err));
        return err;
    }

    err = load_string_config(DEVICE_CONFIG_ID_PROVISIONING_SSID,
                             provisioning_ssid,
                             sizeof(provisioning_ssid));
    if (err == ESP_OK && provisioning_ssid[0] != '\0') {
        wifi_config.provisioning_ssid = provisioning_ssid;
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "SSID de provisionamento indisponivel: %s", esp_err_to_name(err));
    }

    err = load_u32_config(DEVICE_CONFIG_ID_STA_MAX_RETRY, &sta_max_retry);
    if (err == ESP_OK) {
        wifi_config.sta_max_retry = (int)sta_max_retry;
    } else {
        ESP_LOGW(TAG, "Retry STA de configuracao indisponivel: %s", esp_err_to_name(err));
    }

    err = load_i32_config(DEVICE_CONFIG_ID_APSTA_POLICY, &apsta_policy);
    if (err == ESP_OK) {
        err = load_u32_config(DEVICE_CONFIG_ID_APSTA_GRACE_PERIOD_S, &apsta_grace_period_s);
    }
    if (err == ESP_OK) {
        wifi_config.apsta_policy = (wifi_manager_apsta_policy_t)apsta_policy;
        wifi_config.apsta_grace_period_s = apsta_grace_period_s;
    } else {
        ESP_LOGW(TAG, "Politica APSTA indisponivel: %s", esp_err_to_name(err));
    }

    wifi_config.force_provisioning = boot_config_button_is_pressed();
    if (wifi_config.force_provisioning) {
        ESP_LOGW(TAG, "Modo de configuracao Wi-Fi forcado pelo botao de boot");
    }

    err = wifi_manager_start_with_config(&wifi_config);
    if (err != ESP_OK) {
        esp_event_handler_unregister(WIFI_MANAGER_EVENT,
                                     ESP_EVENT_ANY_ID,
                                     &wifi_manager_event_handler);
        return err;
    }

    s_started = true;
    sync_connectivity_outputs(true);
    return ESP_OK;
}
