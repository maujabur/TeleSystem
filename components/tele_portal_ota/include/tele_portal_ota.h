#ifndef TELE_PORTAL_OTA_H
#define TELE_PORTAL_OTA_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

typedef esp_err_t (*tele_portal_ota_begin_cb_t)(void *ctx);
typedef esp_err_t (*tele_portal_ota_write_cb_t)(const uint8_t *data, size_t data_len, void *ctx);
typedef esp_err_t (*tele_portal_ota_finalize_cb_t)(void *ctx);
typedef void (*tele_portal_ota_abort_cb_t)(void *ctx);
typedef esp_err_t (*tele_portal_ota_status_cb_t)(cJSON *json, void *ctx);

typedef struct {
    tele_portal_ota_begin_cb_t begin;
    tele_portal_ota_write_cb_t write;
    tele_portal_ota_finalize_cb_t finalize;
    tele_portal_ota_abort_cb_t abort;
    tele_portal_ota_status_cb_t status;
    void *ctx;
    uint32_t restart_delay_ms;
} tele_portal_ota_config_t;

esp_err_t tele_portal_ota_init(const tele_portal_ota_config_t *config);
esp_err_t tele_portal_ota_register_handlers(httpd_handle_t server);
esp_err_t tele_portal_ota_register_routes(void);

#endif
