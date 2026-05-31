#ifndef WEB_PORTAL_H
#define WEB_PORTAL_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

typedef esp_err_t (*web_portal_routes_register_fn)(httpd_handle_t server);

esp_err_t web_portal_register_app_routes(web_portal_routes_register_fn register_fn);
esp_err_t web_portal_start(bool captive_mode);
esp_err_t web_portal_stop(void);
bool web_portal_is_running(void);

#endif
