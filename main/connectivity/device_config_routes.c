#include <stdbool.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "device_config_routes.h"
#include "device_config_store.h"
#include "http_helpers.h"
#include "web_portal.h"
#include "wifi_manager.h"

#ifndef CONFIG_WEB_PORTAL_EXPOSE_NETWORK_IDENTIFIERS
#define CONFIG_WEB_PORTAL_EXPOSE_NETWORK_IDENTIFIERS 0
#endif

#ifndef CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS
#define CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS 0
#endif

static esp_err_t send_internal_error(httpd_req_t *req, esp_err_t err)
{
    (void)err;
    httpd_resp_set_status(req, "500 Internal Server Error");
    if (CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS) {
        return httpd_resp_sendstr(req, esp_err_to_name(err));
    }
    return httpd_resp_sendstr(req, "Falha interna ao processar solicitacao");
}

static esp_err_t api_device_config_get_handler(httpd_req_t *req)
{
    char provisioning_ssid[DEVICE_CONFIG_PROVISIONING_SSID_BUFFER_SIZE] = {0};
    uint8_t sta_max_retry = 0;
    device_config_apsta_policy_t apsta_policy = DEVICE_CONFIG_APSTA_AUTO_TIMEOUT;
    uint32_t apsta_grace_period_s = 0;
    cJSON *json = cJSON_CreateObject();
    esp_err_t ssid_err = device_config_store_load_provisioning_ssid(provisioning_ssid,
                                                                    sizeof(provisioning_ssid));
    esp_err_t retry_err = device_config_store_load_sta_max_retry(&sta_max_retry);
    esp_err_t apsta_err = device_config_store_load_apsta_policy(&apsta_policy,
                                                                &apsta_grace_period_s);
    esp_err_t err = ssid_err == ESP_OK ? retry_err : ssid_err;

    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    if (ssid_err == ESP_OK) {
        if (CONFIG_WEB_PORTAL_EXPOSE_NETWORK_IDENTIFIERS) {
            cJSON_AddStringToObject(json, "provisioning_ssid", provisioning_ssid);
        } else {
            cJSON_AddStringToObject(json, "provisioning_ssid", "");
            cJSON_AddBoolToObject(json, "provisioning_ssid_hidden", true);
        }
    } else {
        cJSON_AddStringToObject(json, "provisioning_ssid", "");
        cJSON_AddStringToObject(json,
                                "error",
                                CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS ? esp_err_to_name(ssid_err) :
                                "Falha ao carregar configuracao de provisionamento");
    }

    if (retry_err == ESP_OK) {
        cJSON_AddNumberToObject(json, "sta_max_retry", sta_max_retry);
    } else {
        cJSON_AddNumberToObject(json, "sta_max_retry", 0);
        cJSON_AddStringToObject(json,
                                "retry_error",
                                CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS ? esp_err_to_name(retry_err) :
                                "Falha ao carregar retry STA");
    }

    if (apsta_err == ESP_OK) {
        cJSON_AddNumberToObject(json, "apsta_policy", (int)apsta_policy);
        cJSON_AddNumberToObject(json, "apsta_grace_period_s", (double)apsta_grace_period_s);
    } else {
        cJSON_AddNumberToObject(json, "apsta_policy", -1);
        cJSON_AddNumberToObject(json, "apsta_grace_period_s", 0);
        cJSON_AddStringToObject(json,
                                "apsta_error",
                                CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS ? esp_err_to_name(apsta_err) :
                                "Falha ao carregar politica APSTA");
    }

    err = http_helpers_send_json(req,
                                 json,
                                 (ssid_err == ESP_OK && retry_err == ESP_OK && apsta_err == ESP_OK) ? 200 : 500);
    cJSON_Delete(json);
    return err;
}

static esp_err_t api_device_config_post_handler(httpd_req_t *req)
{
    char body[256];
    cJSON *json = NULL;
    const cJSON *provisioning_ssid = NULL;
    const cJSON *sta_max_retry = NULL;
    const cJSON *apsta_policy = NULL;
    const cJSON *apsta_grace_period_s = NULL;
    bool has_update = false;
    esp_err_t err = http_helpers_recv_body(req, body, sizeof(body));

    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Body invalido");
    }

    json = cJSON_Parse(body);
    if (!json) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "JSON invalido");
    }

    provisioning_ssid = cJSON_GetObjectItemCaseSensitive(json, "provisioning_ssid");
    if (cJSON_IsString(provisioning_ssid) &&
        provisioning_ssid->valuestring &&
        provisioning_ssid->valuestring[0] != '\0') {
        err = device_config_store_save_provisioning_ssid(provisioning_ssid->valuestring);
        if (err != ESP_OK) {
            cJSON_Delete(json);
            return send_internal_error(req, err);
        }

        err = wifi_manager_set_provisioning_ssid(provisioning_ssid->valuestring);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            cJSON_Delete(json);
            return send_internal_error(req, err);
        }
        has_update = true;
    } else if (provisioning_ssid) {
        cJSON_Delete(json);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "provisioning_ssid invalido");
    }

    sta_max_retry = cJSON_GetObjectItemCaseSensitive(json, "sta_max_retry");
    if (cJSON_IsNumber(sta_max_retry)) {
        int retry = sta_max_retry->valueint;
        if (retry < DEVICE_CONFIG_STA_MAX_RETRY_MIN || retry > DEVICE_CONFIG_STA_MAX_RETRY_MAX) {
            cJSON_Delete(json);
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "sta_max_retry fora do intervalo permitido");
        }

        err = device_config_store_save_sta_max_retry((uint8_t)retry);
        if (err != ESP_OK) {
            cJSON_Delete(json);
            return send_internal_error(req, err);
        }

        err = wifi_manager_set_sta_max_retry(retry);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            cJSON_Delete(json);
            return send_internal_error(req, err);
        }
        has_update = true;
    } else if (sta_max_retry) {
        cJSON_Delete(json);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "sta_max_retry invalido");
    }

    apsta_policy = cJSON_GetObjectItemCaseSensitive(json, "apsta_policy");
    apsta_grace_period_s = cJSON_GetObjectItemCaseSensitive(json, "apsta_grace_period_s");
    if (apsta_policy || apsta_grace_period_s) {
        int policy = -1;
        uint32_t grace_period_s = 0;

        if (!cJSON_IsNumber(apsta_policy) || !cJSON_IsNumber(apsta_grace_period_s)) {
            cJSON_Delete(json);
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "apsta_policy e apsta_grace_period_s obrigatorios");
        }

        policy = apsta_policy->valueint;
        grace_period_s = (uint32_t)apsta_grace_period_s->valuedouble;
        if (policy < DEVICE_CONFIG_APSTA_ALWAYS_ON ||
            policy > DEVICE_CONFIG_APSTA_STA_ONLY ||
            grace_period_s < DEVICE_CONFIG_APSTA_GRACE_PERIOD_S_MIN ||
            grace_period_s > DEVICE_CONFIG_APSTA_GRACE_PERIOD_S_MAX) {
            cJSON_Delete(json);
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "politica APSTA fora do intervalo permitido");
        }

        err = device_config_store_save_apsta_policy((device_config_apsta_policy_t)policy,
                                                    grace_period_s);
        if (err != ESP_OK) {
            cJSON_Delete(json);
            return send_internal_error(req, err);
        }

        err = wifi_manager_set_apsta_policy((wifi_manager_apsta_policy_t)policy, grace_period_s);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            cJSON_Delete(json);
            return send_internal_error(req, err);
        }
        has_update = true;
    }

    if (!has_update) {
        cJSON_Delete(json);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req,
                                  "informe provisioning_ssid e/ou sta_max_retry e/ou configuracao APSTA");
    }

    cJSON_Delete(json);

    return httpd_resp_sendstr(req,
                              "Configuracao do dispositivo salva. Retry/politica APSTA aplicados e SSID de provisionamento atualizado.");
}

static esp_err_t register_device_config_routes(httpd_handle_t server)
{
    httpd_uri_t api_get = {
        .uri = "/api/device/config",
        .method = HTTP_GET,
        .handler = api_device_config_get_handler,
    };
    httpd_uri_t api_post = {
        .uri = "/api/device/config",
        .method = HTTP_POST,
        .handler = api_device_config_post_handler,
    };
    esp_err_t err = ESP_OK;

    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &api_get);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &api_post);
    }

    return err;
}

esp_err_t device_config_routes_register_with_portal(void)
{
    return web_portal_register_app_routes(register_device_config_routes);
}
