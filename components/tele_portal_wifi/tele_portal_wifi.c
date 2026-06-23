#include "tele_portal_wifi.h"

#include <limits.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "http_helpers.h"
#include "tele_portal_core.h"
#include "wifi_manager.h"

#ifndef CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS
#define CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS 0
#endif

static const char *TAG = "portal-wifi";

void tele_portal_wifi_note_activity(void *ctx)
{
    (void)ctx;
    (void)wifi_manager_note_portal_activity();
}

static esp_err_t send_internal_error(httpd_req_t *req, esp_err_t err)
{
    (void)err;
    httpd_resp_set_status(req, "500 Internal Server Error");
    if (CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS) {
        return httpd_resp_sendstr(req, esp_err_to_name(err));
    }
    return httpd_resp_sendstr(req, "Falha interna ao processar solicitacao");
}

static esp_err_t api_wifi_post_handler(httpd_req_t *req)
{
    char body[256];
    cJSON *json = NULL;
    const cJSON *ssid = NULL;
    const cJSON *password = NULL;
    esp_err_t err = http_helpers_recv_body(req, body, sizeof(body));

    tele_portal_core_note_activity();
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Body invalido");
    }

    json = cJSON_Parse(body);
    if (!json) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "JSON invalido");
    }

    ssid = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    password = cJSON_GetObjectItemCaseSensitive(json, "password");

    if (!cJSON_IsString(ssid) || !ssid->valuestring || !cJSON_IsString(password) || !password->valuestring) {
        cJSON_Delete(json);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Campos ssid/password obrigatorios");
    }

    err = wifi_manager_apply_wifi_credentials(ssid->valuestring, password->valuestring);
    cJSON_Delete(json);
    if (err != ESP_OK) {
        return send_internal_error(req, err);
    }

    return httpd_resp_sendstr(req, "Wi-Fi salvo. Tentando conectar.");
}

static esp_err_t api_wifi_networks_get_handler(httpd_req_t *req)
{
    wifi_manager_network_t networks[WIFI_MANAGER_MAX_SCAN_RESULTS] = {0};
    cJSON *json = cJSON_CreateObject();
    cJSON *array = NULL;
    size_t network_count = 0;
    esp_err_t err = ESP_OK;
    size_t i = 0;

    tele_portal_core_note_activity();
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    err = wifi_manager_scan_networks(networks, WIFI_MANAGER_MAX_SCAN_RESULTS, &network_count);
    if (err == ESP_OK) {
        array = cJSON_AddArrayToObject(json, "networks");
        for (i = 0; i < network_count; ++i) {
            cJSON *item = cJSON_CreateObject();
            if (!item) {
                err = ESP_ERR_NO_MEM;
                break;
            }
            cJSON_AddStringToObject(item, "ssid", networks[i].ssid);
            cJSON_AddNumberToObject(item, "rssi", networks[i].rssi);
            cJSON_AddBoolToObject(item, "auth_required", networks[i].auth_required);
            cJSON_AddItemToArray(array, item);
        }
    } else {
        cJSON_AddStringToObject(json,
                                "error",
                                CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS ? esp_err_to_name(err) :
                                "Falha ao escanear redes");
    }

    err = http_helpers_send_json(req, json, err == ESP_OK ? 200 : 500);
    cJSON_Delete(json);
    return err;
}

static esp_err_t api_wifi_saved_list_get_handler(httpd_req_t *req)
{
    wifi_manager_saved_network_t saved[WIFI_MANAGER_MAX_SAVED_NETWORKS] = {0};
    cJSON *json = cJSON_CreateObject();
    cJSON *array = NULL;
    size_t saved_count = 0;
    size_t i = 0;
    esp_err_t err = ESP_OK;

    tele_portal_core_note_activity();
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    err = wifi_manager_list_saved_networks(saved,
                                           WIFI_MANAGER_MAX_SAVED_NETWORKS,
                                           &saved_count);
    if (err == ESP_OK) {
        array = cJSON_AddArrayToObject(json, "saved_networks");
        for (i = 0; i < saved_count; ++i) {
            cJSON *item = cJSON_CreateObject();
            if (!item) {
                err = ESP_ERR_NO_MEM;
                break;
            }
            cJSON_AddStringToObject(item, "ssid", saved[i].ssid);
            cJSON_AddNumberToObject(item, "priority", saved[i].priority);
            cJSON_AddItemToArray(array, item);
        }
    } else if (err == ESP_ERR_NOT_FOUND) {
        cJSON_AddArrayToObject(json, "saved_networks");
        err = ESP_OK;
    } else {
        cJSON_AddStringToObject(json,
                                "error",
                                CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS ? esp_err_to_name(err) :
                                "Falha ao listar redes salvas");
    }

    err = http_helpers_send_json(req, json, err == ESP_OK ? 200 : 500);
    cJSON_Delete(json);
    return err;
}

static esp_err_t api_wifi_saved_update_put_handler(httpd_req_t *req)
{
    char body[160];
    cJSON *json = NULL;
    const cJSON *ssid = NULL;
    const cJSON *priority = NULL;
    int32_t parsed_priority = 0;
    esp_err_t err = http_helpers_recv_body(req, body, sizeof(body));

    tele_portal_core_note_activity();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body invalido");
    }

    json = cJSON_Parse(body);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON invalido");
    }

    ssid = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    priority = cJSON_GetObjectItemCaseSensitive(json, "priority");
    if (!cJSON_IsString(ssid) || !ssid->valuestring || ssid->valuestring[0] == '\0') {
        cJSON_Delete(json);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Campo ssid obrigatorio");
    }
    if (!cJSON_IsNumber(priority)) {
        cJSON_Delete(json);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Campo priority obrigatorio");
    }
    if (priority->valuedouble < (double)INT32_MIN ||
        priority->valuedouble > (double)INT32_MAX ||
        priority->valuedouble != (double)((int32_t)priority->valuedouble)) {
        cJSON_Delete(json);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "priority deve ser int32");
    }

    parsed_priority = (int32_t)priority->valuedouble;
    err = wifi_manager_set_saved_network_priority(ssid->valuestring, parsed_priority);
    cJSON_Delete(json);

    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Rede salva nao encontrada");
    }
    if (err != ESP_OK) {
        return send_internal_error(req, err);
    }

    return httpd_resp_sendstr(req, "Prioridade da rede atualizada");
}

static esp_err_t api_wifi_saved_delete_handler(httpd_req_t *req)
{
    char body[128];
    cJSON *json = NULL;
    const cJSON *ssid = NULL;
    esp_err_t err = http_helpers_recv_body(req, body, sizeof(body));

    tele_portal_core_note_activity();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body invalido");
    }

    json = cJSON_Parse(body);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON invalido");
    }

    ssid = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    if (!cJSON_IsString(ssid) || !ssid->valuestring || ssid->valuestring[0] == '\0') {
        cJSON_Delete(json);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Campo ssid obrigatorio");
    }

    err = wifi_manager_remove_saved_network(ssid->valuestring);
    cJSON_Delete(json);

    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Rede salva nao encontrada");
    }
    if (err != ESP_OK) {
        return send_internal_error(req, err);
    }

    return httpd_resp_sendstr(req, "Rede salva removida");
}

static esp_err_t register_uri_checked(httpd_handle_t server, const httpd_uri_t *route)
{
    esp_err_t err = httpd_register_uri_handler(server, route);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar rota %s %s: %s",
                 route->method == HTTP_GET ? "GET" :
                 route->method == HTTP_POST ? "POST" :
                 route->method == HTTP_PUT ? "PUT" :
                 route->method == HTTP_DELETE ? "DELETE" : "HTTP",
                 route->uri,
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t tele_portal_wifi_register_routes(httpd_handle_t server)
{
    httpd_uri_t api_wifi = {
        .uri = "/api/wifi",
        .method = HTTP_POST,
        .handler = api_wifi_post_handler,
    };
    httpd_uri_t api_wifi_networks = {
        .uri = "/api/wifi/networks",
        .method = HTTP_GET,
        .handler = api_wifi_networks_get_handler,
    };
    httpd_uri_t api_wifi_saved_list = {
        .uri = "/api/wifi/saved",
        .method = HTTP_GET,
        .handler = api_wifi_saved_list_get_handler,
    };
    httpd_uri_t api_wifi_saved_update = {
        .uri = "/api/wifi/saved",
        .method = HTTP_PUT,
        .handler = api_wifi_saved_update_put_handler,
    };
    httpd_uri_t api_wifi_saved_delete = {
        .uri = "/api/wifi/saved",
        .method = HTTP_DELETE,
        .handler = api_wifi_saved_delete_handler,
    };

    esp_err_t err = register_uri_checked(server, &api_wifi);
    if (err == ESP_OK) {
        err = register_uri_checked(server, &api_wifi_networks);
    }
    if (err == ESP_OK) {
        err = register_uri_checked(server, &api_wifi_saved_list);
    }
    if (err == ESP_OK) {
        err = register_uri_checked(server, &api_wifi_saved_update);
    }
    if (err == ESP_OK) {
        err = register_uri_checked(server, &api_wifi_saved_delete);
    }
    return err;
}
