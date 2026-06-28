#ifndef TELE_FIRMWARE_PORTAL_OTA_H
#define TELE_FIRMWARE_PORTAL_OTA_H

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t tele_firmware_portal_ota_register_handlers(httpd_handle_t server);
esp_err_t tele_firmware_portal_ota_register_routes(void);

#endif
