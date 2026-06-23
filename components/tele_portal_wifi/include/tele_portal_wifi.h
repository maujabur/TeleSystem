#ifndef TELE_PORTAL_WIFI_H
#define TELE_PORTAL_WIFI_H

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t tele_portal_wifi_register_routes(httpd_handle_t server);
void tele_portal_wifi_note_activity(void *ctx);

#endif
