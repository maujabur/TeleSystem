#ifndef TELE_PORTAL_ASSETS_H
#define TELE_PORTAL_ASSETS_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

typedef struct {
    bool enable_logs_page;
} tele_portal_assets_config_t;

esp_err_t tele_portal_assets_init(const tele_portal_assets_config_t *config);
esp_err_t tele_portal_assets_register_routes(httpd_handle_t server);
esp_err_t tele_portal_assets_root_handler(httpd_req_t *req);

#endif
