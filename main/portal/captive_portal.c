#include "captive_portal.h"

#include <stddef.h>

#include "dns_server.h"
#include "esp_http_server.h"
#include "lwip/ip4_addr.h"

#include "wifi_manager.h"

static const char *CAPTIVE_PORTAL_BASE_URL = "http://192.168.42.1/";

static bool s_captive_mode;
static captive_portal_root_handler_fn s_root_handler;

static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    (void)wifi_manager_note_portal_activity();

    if (!s_captive_mode && s_root_handler) {
        return s_root_handler(req);
    }

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", CAPTIVE_PORTAL_BASE_URL);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t captive_not_found_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)wifi_manager_note_portal_activity();

    if (!s_captive_mode) {
        return httpd_resp_send_err(req, err, "Pagina nao encontrada");
    }

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", CAPTIVE_PORTAL_BASE_URL);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t register_redirect_route(httpd_handle_t server, const char *uri)
{
    httpd_uri_t route = {
        .uri = uri,
        .method = HTTP_GET,
        .handler = captive_redirect_handler,
    };

    return httpd_register_uri_handler(server, &route);
}

esp_err_t captive_portal_register_handlers(httpd_handle_t server,
                                           captive_portal_root_handler_fn root_handler)
{
    static const char *uris[] = {
        "/generate_204",
        "/hotspot-detect.html",
        "/connecttest.txt",
        "/gen_204",
        "/ncsi.txt",
        "/online",
        "/success.txt",
        "/success.html",
        "/mobile/status.php",
        "/fwlink",
    };

    if (!server || !root_handler) {
        return ESP_ERR_INVALID_ARG;
    }

    s_root_handler = root_handler;
    esp_err_t err = httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, captive_not_found_handler);
    if (err != ESP_OK) {
        return err;
    }

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); ++i) {
        err = register_redirect_route(server, uris[i]);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

esp_err_t captive_portal_set_enabled(bool enabled)
{
    s_captive_mode = enabled;

    if (!enabled) {
        return dns_server_stop();
    }

    esp_ip4_addr_t portal_ip;
    IP4_ADDR(&portal_ip, 192, 168, 42, 1);
    return dns_server_start(portal_ip);
}

esp_err_t captive_portal_stop(void)
{
    s_captive_mode = false;
    return dns_server_stop();
}
