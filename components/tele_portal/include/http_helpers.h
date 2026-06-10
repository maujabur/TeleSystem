#ifndef HTTP_HELPERS_H
#define HTTP_HELPERS_H

#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t http_helpers_send_json(httpd_req_t *req, cJSON *json, int status_code);
esp_err_t http_helpers_recv_body(httpd_req_t *req, char *buffer, size_t buffer_size);
esp_err_t http_helpers_send_file(httpd_req_t *req, const char *content_type, const char *path);

#endif
