#ifndef CAPTIVE_PORTAL_H
#define CAPTIVE_PORTAL_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

typedef esp_err_t (*captive_portal_root_handler_fn)(httpd_req_t *req);

esp_err_t captive_portal_register_handlers(httpd_handle_t server,
                                           captive_portal_root_handler_fn root_handler);
esp_err_t captive_portal_set_enabled(bool enabled);
esp_err_t captive_portal_stop(void);

#endif
