#ifndef TELE_PORTAL_CORE_H
#define TELE_PORTAL_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

typedef esp_err_t (*tele_portal_core_routes_register_fn)(httpd_handle_t server);
typedef void (*tele_portal_core_activity_cb_t)(void *ctx);
typedef void (*tele_portal_core_restart_cb_t)(uint32_t delay_ms, void *ctx);

typedef struct {
    size_t max_uri_handlers;
    size_t max_open_sockets;
    uint32_t socket_timeout_s;
    tele_portal_core_activity_cb_t on_activity;
    tele_portal_core_restart_cb_t restart;
    void *ctx;
} tele_portal_core_config_t;

esp_err_t tele_portal_core_init(const tele_portal_core_config_t *config);
esp_err_t tele_portal_core_register_routes(tele_portal_core_routes_register_fn register_fn);
esp_err_t tele_portal_core_start(bool captive_mode);
esp_err_t tele_portal_core_stop(void);
bool tele_portal_core_is_running(void);
void tele_portal_core_note_activity(void);
void tele_portal_core_request_restart(uint32_t delay_ms);

#endif
