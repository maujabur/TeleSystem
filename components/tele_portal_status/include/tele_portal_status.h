#ifndef TELE_PORTAL_STATUS_H
#define TELE_PORTAL_STATUS_H

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t tele_portal_status_register_routes(httpd_handle_t server);

#endif
