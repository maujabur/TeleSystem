#ifndef TELE_PORTAL_CAPTIVE_H
#define TELE_PORTAL_CAPTIVE_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

typedef esp_err_t (*tele_portal_captive_root_handler_fn)(httpd_req_t *req);

esp_err_t tele_portal_captive_register_handlers(httpd_handle_t server,
                                                tele_portal_captive_root_handler_fn root_handler);
esp_err_t tele_portal_captive_set_enabled(bool enabled);
esp_err_t tele_portal_captive_stop(void);

#endif
