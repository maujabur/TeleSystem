#ifndef TELE_PORTAL_CONFIG_H
#define TELE_PORTAL_CONFIG_H

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t tele_portal_config_register_routes(httpd_handle_t server);

#endif
