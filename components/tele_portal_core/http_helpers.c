#include "http_helpers.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "tele_portal_core.h"

static const char *TAG = "http-helpers";

static const char *status_text_from_code(int status_code)
{
    switch (status_code) {
    case 200:
        return "200 OK";
    case 400:
        return "400 Bad Request";
    case 404:
        return "404 Not Found";
    case 500:
        return "500 Internal Server Error";
    default:
        return "200 OK";
    }
}

esp_err_t http_helpers_send_json(httpd_req_t *req, cJSON *json, int status_code)
{
    char *payload = NULL;

    if (!req || !json) {
        return ESP_ERR_INVALID_ARG;
    }

    tele_portal_core_note_activity();

    payload = cJSON_PrintUnformatted(json);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_status(req, status_text_from_code(status_code));
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, payload, strlen(payload));
    cJSON_free(payload);
    return err;
}

esp_err_t http_helpers_recv_body(httpd_req_t *req, char *buffer, size_t buffer_size)
{
    int total_received = 0;

    if (!req || !buffer || buffer_size == 0 || req->content_len <= 0 ||
        req->content_len >= (int)buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    tele_portal_core_note_activity();

    while (total_received < req->content_len) {
        int received = httpd_req_recv(req,
                                      buffer + total_received,
                                      req->content_len - total_received);
        if (received <= 0) {
            return ESP_FAIL;
        }
        total_received += received;
    }

    buffer[total_received] = '\0';
    return ESP_OK;
}

esp_err_t http_helpers_send_file(httpd_req_t *req, const char *content_type, const char *path)
{
    char buffer[1024];
    FILE *file = NULL;

    if (!req || !content_type || !path) {
        return ESP_ERR_INVALID_ARG;
    }

    tele_portal_core_note_activity();

    file = fopen(path, "rb");
    httpd_resp_set_type(req, content_type);
    if (!file) {
        ESP_LOGE(TAG, "Nao foi possivel abrir %s", path);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Falha ao abrir arquivo");
    }

    while (!feof(file)) {
        size_t bytes_read = fread(buffer, 1, sizeof(buffer), file);
        if (bytes_read > 0) {
            esp_err_t err = httpd_resp_send_chunk(req, buffer, bytes_read);
            if (err != ESP_OK) {
                fclose(file);
                return err;
            }
        }

        if (ferror(file)) {
            fclose(file);
            ESP_LOGE(TAG, "Erro ao ler %s", path);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Falha ao ler arquivo");
        }
    }

    fclose(file);
    return httpd_resp_send_chunk(req, NULL, 0);
}
