#include <stdbool.h>
#include <stdlib.h>
#include "esp_system.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tele_portal_assets.h"
#include "tele_portal_captive.h"
#include "tele_portal_config.h"
#include "tele_portal_core.h"
#include "tele_portal_logs.h"
#include "tele_portal_status.h"
#include "tele_portal_wifi.h"
#include "web_portal.h"

#define WEB_PORTAL_MAX_URI_HANDLERS 48
#define WEB_PORTAL_MAX_OPEN_SOCKETS 4
#define WEB_PORTAL_SOCKET_TIMEOUT_SECONDS 5

static const char *TAG = "web-portal";
static bool s_core_initialized;
static bool s_base_routes_registered;

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_LOGW(TAG, "Reiniciando placa por requisicao web");
    esp_restart();
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
        .on_activity = tele_portal_wifi_note_activity,
        .restart = portal_restart_cb,
    };
    esp_err_t err = tele_portal_core_init(&config);
    if (err == ESP_OK) {
        tele_portal_assets_config_t assets_config = {
            .enable_logs_page = tele_portal_logs_endpoint_enabled(),
        };
        ESP_ERROR_CHECK(tele_portal_assets_init(&assets_config));
        s_core_initialized = true;
    }
    return err;
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
    httpd_uri_t api_restart = {
        .uri = "/api/restart",
        .method = HTTP_POST,
        .handler = api_restart_post_handler,
    };

    esp_err_t err = tele_portal_assets_register_routes(server);
    if (err == ESP_OK) {
        err = tele_portal_logs_register_routes(server);
    }
    if (err == ESP_OK) {
        err = tele_portal_status_register_routes(server);
    }
    if (err == ESP_OK) {
        err = tele_portal_config_register_routes(server);
    }
    if (err == ESP_OK) {
        err = register_uri_checked(server, &api_restart);
    }
    if (err == ESP_OK) {
        err = tele_portal_wifi_register_routes(server);
    }

    if (err != ESP_OK) {
        return err;
    }

    err = tele_portal_captive_register_handlers(server, tele_portal_assets_root_handler);
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

    err = tele_portal_captive_set_enabled(captive_mode);
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

    tele_portal_captive_stop();
    return tele_portal_core_stop();
}

bool web_portal_is_running(void)
{
    return tele_portal_core_is_running();
}
