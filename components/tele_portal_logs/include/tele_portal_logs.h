#ifndef TELE_PORTAL_LOGS_H
#define TELE_PORTAL_LOGS_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

bool tele_portal_logs_endpoint_enabled(void);
void tele_portal_logs_init(void);
size_t tele_portal_logs_get_snapshot(char *out, size_t out_size);
esp_err_t tele_portal_logs_register_routes(httpd_handle_t server);

#endif
