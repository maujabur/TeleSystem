#include "tele_portal_assets.h"

#include <sys/types.h>

#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "portal-assets";

static bool s_enable_logs_page;

extern const unsigned char _binary_index_html_start[];
extern const unsigned char _binary_index_html_end[];
extern const unsigned char _binary_app_css_start[];
extern const unsigned char _binary_app_css_end[];
extern const unsigned char _binary_portal_css_start[];
extern const unsigned char _binary_portal_css_end[];
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

static esp_err_t send_embedded_css(httpd_req_t *req,
                                   const unsigned char *start,
                                   const unsigned char *end)
{
    size_t len = (size_t)(end - start);
    httpd_resp_set_type(req, "text/css; charset=utf-8");
    return httpd_resp_send(req, (const char *)start, (ssize_t)len);
}

static esp_err_t register_uri_checked(httpd_handle_t server, const httpd_uri_t *route)
{
    esp_err_t err = httpd_register_uri_handler(server, route);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar asset %s: %s", route->uri, esp_err_to_name(err));
    }
    return err;
}

esp_err_t tele_portal_assets_init(const tele_portal_assets_config_t *config)
{
    s_enable_logs_page = config ? config->enable_logs_page : false;
    return ESP_OK;
}

esp_err_t tele_portal_assets_root_handler(httpd_req_t *req)
{
    return send_embedded_html(req, _binary_index_html_start, _binary_index_html_end);
}

static esp_err_t logs_page_get_handler(httpd_req_t *req)
{
    if (!s_enable_logs_page) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Pagina nao encontrada");
    }

    return send_embedded_html(req, _binary_logs_html_start, _binary_logs_html_end);
}

static esp_err_t app_css_get_handler(httpd_req_t *req)
{
    return send_embedded_css(req, _binary_app_css_start, _binary_app_css_end);
}

static esp_err_t portal_css_get_handler(httpd_req_t *req)
{
    return send_embedded_css(req, _binary_portal_css_start, _binary_portal_css_end);
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

esp_err_t tele_portal_assets_register_routes(httpd_handle_t server)
{
    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = tele_portal_assets_root_handler,
    };
    httpd_uri_t logs_page = {
        .uri = "/logs",
        .method = HTTP_GET,
        .handler = logs_page_get_handler,
    };
    httpd_uri_t app_css = {
        .uri = "/app.css",
        .method = HTTP_GET,
        .handler = app_css_get_handler,
    };
    httpd_uri_t portal_css = {
        .uri = "/portal.css",
        .method = HTTP_GET,
        .handler = portal_css_get_handler,
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

    esp_err_t err = register_uri_checked(server, &root);
    if (err == ESP_OK) {
        err = register_uri_checked(server, &app_css);
    }
    if (err == ESP_OK) {
        err = register_uri_checked(server, &portal_css);
    }
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

    return err;
}
