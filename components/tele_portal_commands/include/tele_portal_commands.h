#ifndef TELE_PORTAL_COMMANDS_H
#define TELE_PORTAL_COMMANDS_H

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t tele_portal_commands_register_routes(httpd_handle_t server);

#endif
