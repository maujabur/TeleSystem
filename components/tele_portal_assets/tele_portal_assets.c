#include "tele_portal_assets.h"
#include "tele_portal_assets_generated.h"

#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "portal-assets";

static bool s_enable_logs_page;

static esp_err_t send_embedded_asset(httpd_req_t *req,
                                     const unsigned char *start,
                                     const unsigned char *end,
                                     const char *content_type)
{
    size_t len = (size_t)(end - start);

    httpd_resp_set_type(req, content_type);

    return httpd_resp_send(req, (const char *)start, (ssize_t)len);
}

static const tele_portal_asset_t *find_asset_by_uri(const char *uri)
{
    for (size_t i = 0; i < tele_portal_assets_count; i++) {
        const tele_portal_asset_t *asset = &tele_portal_assets[i];

        if (asset->requires_logs_enabled && !s_enable_logs_page) {
            continue;
        }

        if (strcmp(asset->uri, uri) == 0) {
            return asset;
        }
    }

    return NULL;
}

static esp_err_t asset_get_handler(httpd_req_t *req)
{
    const tele_portal_asset_t *asset = find_asset_by_uri(req->uri);

    if (!asset && strcmp(req->uri, "/") != 0) {
        char html_uri[128];

        int written = snprintf(
            html_uri,
            sizeof(html_uri),
            "%s.html",
            req->uri
        );

        if (written > 0 && written < (int)sizeof(html_uri)) {
            asset = find_asset_by_uri(html_uri);
        }
    }

    if (!asset) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    return send_embedded_asset(
        req,
        asset->start,
        asset->end,
        asset->content_type
    );
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
    const tele_portal_asset_t *asset = find_asset_by_uri("/");

    if (!asset) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    return send_embedded_asset(
        req,
        asset->start,
        asset->end,
        asset->content_type
    );
}

esp_err_t tele_portal_assets_register_routes(httpd_handle_t server)
{
    httpd_uri_t assets = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = asset_get_handler,
        .user_ctx = NULL,
    };

    return register_uri_checked(server, &assets);
}