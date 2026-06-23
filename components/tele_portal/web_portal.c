#include <stdbool.h>
#include <stdlib.h>
#include <limits.h>
#include "cJSON.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_log_buffer.h"
#include "captive_portal.h"
#include "firmware_version.h"
#include "http_helpers.h"
#include "time_sync.h"
#include "tele_portal_core.h"
#include "vbat_monitor.h"
#include "web_portal.h"
#include "wifi_manager.h"

#define WEB_PORTAL_MAX_URI_HANDLERS 48
#define WEB_PORTAL_MAX_OPEN_SOCKETS 4
#define WEB_PORTAL_SOCKET_TIMEOUT_SECONDS 5

#ifndef CONFIG_WEB_PORTAL_ENABLE_LOGS_ENDPOINT
#define CONFIG_WEB_PORTAL_ENABLE_LOGS_ENDPOINT 0
#endif

#ifndef CONFIG_WEB_PORTAL_EXPOSE_NETWORK_IDENTIFIERS
#define CONFIG_WEB_PORTAL_EXPOSE_NETWORK_IDENTIFIERS 0
#endif

#ifndef CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS
#define CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS 0
#endif

#ifndef CONFIG_POWER_GOOD_GPIO_ENABLED
#define CONFIG_POWER_GOOD_GPIO_ENABLED 0
#endif

#ifndef CONFIG_POWER_GOOD_GPIO
#define CONFIG_POWER_GOOD_GPIO 6
#endif

#ifndef CONFIG_POWER_GOOD_ACTIVE_LEVEL
#define CONFIG_POWER_GOOD_ACTIVE_LEVEL 1
#endif

static const char *TAG = "web-portal";
static bool s_core_initialized;
static bool s_base_routes_registered;

extern const unsigned char _binary_index_html_start[];
extern const unsigned char _binary_index_html_end[];
extern const unsigned char _binary_status_html_start[];
extern const unsigned char _binary_status_html_end[];
extern const unsigned char _binary_settings_html_start[];
extern const unsigned char _binary_settings_html_end[];
extern const unsigned char _binary_networks_html_start[];
extern const unsigned char _binary_networks_html_end[];
extern const unsigned char _binary_logs_html_start[];
extern const unsigned char _binary_logs_html_end[];

static esp_err_t send_embedded_html(httpd_req_t *req,
                                    const unsigned char *start,
                                    const unsigned char *end)
{
    size_t len = (size_t)(end - start);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)start, (ssize_t)len);
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

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_LOGW(TAG, "Reiniciando placa por requisicao web");
    esp_restart();
}

static void portal_activity_cb(void *ctx)
{
    (void)ctx;
    (void)wifi_manager_note_portal_activity();
}

static void portal_restart_cb(uint32_t delay_ms, void *ctx)
{
    (void)delay_ms;
    (void)ctx;
    xTaskCreate(restart_task, "web_restart", 2048, NULL, 5, NULL);
}

static esp_err_t ensure_core_initialized(void)
{
    if (s_core_initialized) {
        return ESP_OK;
    }

    tele_portal_core_config_t config = {
        .max_uri_handlers = WEB_PORTAL_MAX_URI_HANDLERS,
        .max_open_sockets = WEB_PORTAL_MAX_OPEN_SOCKETS,
        .socket_timeout_s = WEB_PORTAL_SOCKET_TIMEOUT_SECONDS,
        .on_activity = portal_activity_cb,
        .restart = portal_restart_cb,
    };
    esp_err_t err = tele_portal_core_init(&config);
    if (err == ESP_OK) {
        s_core_initialized = true;
    }
    return err;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    return send_embedded_html(req, _binary_index_html_start, _binary_index_html_end);
}

static esp_err_t logs_page_get_handler(httpd_req_t *req)
{
    if (!CONFIG_WEB_PORTAL_ENABLE_LOGS_ENDPOINT) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Pagina nao encontrada");
    }

    return send_embedded_html(req, _binary_logs_html_start, _binary_logs_html_end);
}

static esp_err_t status_page_get_handler(httpd_req_t *req)
{
    return send_embedded_html(req, _binary_status_html_start, _binary_status_html_end);
}

static esp_err_t settings_page_get_handler(httpd_req_t *req)
{
    return send_embedded_html(req, _binary_settings_html_start, _binary_settings_html_end);
}

static esp_err_t networks_page_get_handler(httpd_req_t *req)
{
    return send_embedded_html(req, _binary_networks_html_start, _binary_networks_html_end);
}

static esp_err_t api_logs_get_handler(httpd_req_t *req)
{
    static char logs[8192];

    if (!CONFIG_WEB_PORTAL_ENABLE_LOGS_ENDPOINT) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Endpoint indisponivel");
    }

    tele_portal_core_note_activity();
    app_log_buffer_get_snapshot(logs, sizeof(logs));
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, logs);
}

static esp_err_t api_restart_post_handler(httpd_req_t *req)
{
    tele_portal_core_note_activity();
    ESP_LOGW(TAG, "Restart solicitado pela interface web");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    esp_err_t err = httpd_resp_sendstr(req, "Reiniciando placa.");
    tele_portal_core_request_restart(300);
    return err;
}

static esp_err_t api_status_get_handler(httpd_req_t *req)
{
    wifi_manager_status_t status;
    vbat_monitor_status_t vbat_status = {0};
    int64_t now_ms = (int64_t)esp_log_timestamp();
    char local_now[32] = {0};
    char utc_now[32] = {0};
    int64_t uptime_ms = now_ms;
    cJSON *json = cJSON_CreateObject();
    cJSON *vbat_json = NULL;
    cJSON *power_good_json = NULL;
    esp_err_t err = wifi_manager_get_status(&status);

    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(json, "firmware_version", APP_VERSION_STRING);
    cJSON_AddNumberToObject(json, "uptime_ms", (double)uptime_ms);
    cJSON_AddNumberToObject(json, "uptime_seconds", (double)(uptime_ms / 1000));
    power_good_json = cJSON_AddObjectToObject(json, "power_good");
    if (power_good_json) {
        cJSON_AddBoolToObject(power_good_json, "enabled", CONFIG_POWER_GOOD_GPIO_ENABLED);
        cJSON_AddNumberToObject(power_good_json, "gpio", CONFIG_POWER_GOOD_GPIO);
        cJSON_AddNumberToObject(power_good_json, "active_level", CONFIG_POWER_GOOD_ACTIVE_LEVEL);
    }
    cJSON_AddBoolToObject(json, "time_synchronized", time_sync_is_synchronized());
    cJSON_AddStringToObject(json, "timezone", time_sync_timezone());
    if (time_sync_format_local_now(local_now, sizeof(local_now))) {
        cJSON_AddStringToObject(json, "local_time_iso8601", local_now);
    }
    if (time_sync_format_utc_now(utc_now, sizeof(utc_now))) {
        cJSON_AddStringToObject(json, "utc_time_iso8601", utc_now);
    }

    if (err == ESP_OK) {
        cJSON_AddStringToObject(json, "state",
                                status.state == WIFI_MANAGER_STATE_STA_CONNECTED ? "sta_connected" :
                                status.state == WIFI_MANAGER_STATE_STA_CONNECTING ? "sta_connecting" :
                                status.state == WIFI_MANAGER_STATE_PROVISIONING_AP ? "provisioning_ap" : "init");
        cJSON_AddBoolToObject(json, "wifi_ready", status.wifi_ready);
        cJSON_AddBoolToObject(json, "provisioning_active", status.provisioning_active);
        if (CONFIG_WEB_PORTAL_EXPOSE_NETWORK_IDENTIFIERS) {
            cJSON_AddStringToObject(json, "ip", status.ip);
            cJSON_AddStringToObject(json, "ssid", status.ssid);
            cJSON_AddStringToObject(json, "provisioning_ssid", status.provisioning_ssid);
        } else {
            cJSON_AddBoolToObject(json, "network_identifiers_hidden", true);
        }
        cJSON_AddNumberToObject(json, "wifi_rssi", status.rssi);
        cJSON_AddNumberToObject(json, "wifi_sta_max_retry", status.sta_max_retry);
        cJSON_AddNumberToObject(json, "wifi_reconnect_attempts", (double)status.sta_reconnect_attempts);
        cJSON_AddNumberToObject(json, "wifi_invalid_transition_count", (double)status.invalid_transition_count);
        cJSON_AddNumberToObject(json, "wifi_apsta_policy", (double)status.apsta_policy);
        cJSON_AddNumberToObject(json, "wifi_apsta_grace_period_s", (double)status.apsta_grace_period_s);
        cJSON_AddBoolToObject(json, "wifi_apsta_auto_drop_pending", status.apsta_auto_drop_pending);
        cJSON_AddStringToObject(json,
                                "last_error",
                                CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS ? status.last_error : "");
    } else {
        cJSON_AddStringToObject(json,
                                "error",
                                CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS ? esp_err_to_name(err) :
                                "Falha ao carregar status");
    }

    if (vbat_monitor_get_status(&vbat_status) == ESP_OK) {
        vbat_json = cJSON_AddObjectToObject(json, "vbat");
        if (vbat_json) {
            cJSON_AddBoolToObject(vbat_json, "enabled", vbat_status.enabled);
            cJSON_AddBoolToObject(vbat_json, "initialized", vbat_status.initialized);
            cJSON_AddBoolToObject(vbat_json, "calibrated", vbat_status.calibrated);
            cJSON_AddNumberToObject(vbat_json, "gpio", vbat_status.gpio);
            cJSON_AddNumberToObject(vbat_json, "raw_avg", vbat_status.raw_avg);
            cJSON_AddNumberToObject(vbat_json, "gpio_mv", vbat_status.gpio_mv);
            cJSON_AddNumberToObject(vbat_json, "vbat_mv", vbat_status.vbat_mv);
            cJSON_AddNumberToObject(vbat_json,
                                    "last_measurement_ms",
                                    (double)vbat_status.last_measurement_ms);
            cJSON_AddStringToObject(vbat_json,
                                    "last_moment",
                                    vbat_monitor_moment_name(vbat_status.last_moment));
            cJSON_AddNumberToObject(vbat_json,
                                    "measurement_count",
                                    (double)vbat_status.measurement_count);
            cJSON_AddBoolToObject(vbat_json,
                                  "shutdown_enabled",
                                  vbat_status.shutdown_enabled);
            cJSON_AddNumberToObject(vbat_json,
                                    "shutdown_threshold_mv",
                                    vbat_status.shutdown_threshold_mv);
            cJSON_AddNumberToObject(vbat_json,
                                    "shutdown_debounce_ms",
                                    vbat_status.shutdown_debounce_ms);
            cJSON_AddBoolToObject(vbat_json,
                                  "shutdown_countdown_active",
                                  vbat_status.shutdown_countdown_active);
            cJSON_AddNumberToObject(vbat_json,
                                    "shutdown_below_threshold_since_ms",
                                    (double)vbat_status.shutdown_below_threshold_since_ms);
            cJSON_AddNumberToObject(vbat_json,
                                    "shutdown_countdown_elapsed_ms",
                                    vbat_status.shutdown_countdown_active &&
                                    vbat_status.shutdown_below_threshold_since_ms > 0 ?
                                        (double)(now_ms - vbat_status.shutdown_below_threshold_since_ms) :
                                        0.0);
        }
    }

    err = http_helpers_send_json(req, json, err == ESP_OK ? 200 : 500);
    cJSON_Delete(json);
    return err;
}

static esp_err_t api_wifi_post_handler(httpd_req_t *req)
{
    char body[256];
    cJSON *json = NULL;
    const cJSON *ssid = NULL;
    const cJSON *password = NULL;
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

esp_err_t web_portal_register_app_routes(web_portal_routes_register_fn register_fn)
{
    esp_err_t err = ensure_core_initialized();
    if (err != ESP_OK) {
        return err;
    }
    return tele_portal_core_register_routes(register_fn);
}

static esp_err_t register_base_routes(httpd_handle_t server)
{
    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    httpd_uri_t logs_page = {
        .uri = "/logs",
        .method = HTTP_GET,
        .handler = logs_page_get_handler,
    };
    httpd_uri_t status_page = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_page_get_handler,
    };
    httpd_uri_t settings_page = {
        .uri = "/settings",
        .method = HTTP_GET,
        .handler = settings_page_get_handler,
    };
    httpd_uri_t networks_page = {
        .uri = "/networks",
        .method = HTTP_GET,
        .handler = networks_page_get_handler,
    };
    httpd_uri_t api_status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = api_status_get_handler,
    };
    httpd_uri_t api_logs = {
        .uri = "/api/logs",
        .method = HTTP_GET,
        .handler = api_logs_get_handler,
    };
    httpd_uri_t api_restart = {
        .uri = "/api/restart",
        .method = HTTP_POST,
        .handler = api_restart_post_handler,
    };
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

    esp_err_t err = register_uri_checked(server, &root);
    if (err == ESP_OK) {
        err = register_uri_checked(server, &logs_page);
    }
    if (err == ESP_OK) {
        err = register_uri_checked(server, &status_page);
    }
    if (err == ESP_OK) {
        err = register_uri_checked(server, &settings_page);
    }
    if (err == ESP_OK) {
        err = register_uri_checked(server, &networks_page);
    }
    if (err == ESP_OK) {
        err = register_uri_checked(server, &api_status);
    }
    if (err == ESP_OK && CONFIG_WEB_PORTAL_ENABLE_LOGS_ENDPOINT) {
        err = register_uri_checked(server, &api_logs);
    }
    if (err == ESP_OK) {
        err = register_uri_checked(server, &api_restart);
    }
    if (err == ESP_OK) {
        err = register_uri_checked(server, &api_wifi);
    }
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

    if (err != ESP_OK) {
        return err;
    }

    err = captive_portal_register_handlers(server, root_get_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar rotas de captive portal: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t web_portal_start(bool captive_mode)
{
    esp_err_t err = ensure_core_initialized();
    if (err != ESP_OK) {
        return err;
    }

    if (!s_base_routes_registered) {
        err = tele_portal_core_register_routes(register_base_routes);
        if (err != ESP_OK) {
            return err;
        }
        s_base_routes_registered = true;
    }

    err = tele_portal_core_start(captive_mode);
    if (err != ESP_OK) {
        return err;
    }

    err = captive_portal_set_enabled(captive_mode);
    if (err != ESP_OK) {
        (void)tele_portal_core_stop();
        return err;
    }

    ESP_LOGI(TAG, "Portal web iniciado (%s)", captive_mode ? "captive" : "normal");
    return ESP_OK;
}

esp_err_t web_portal_stop(void)
{
    if (!tele_portal_core_is_running()) {
        return ESP_OK;
    }

    captive_portal_stop();
    return tele_portal_core_stop();
}

bool web_portal_is_running(void)
{
    return tele_portal_core_is_running();
}
