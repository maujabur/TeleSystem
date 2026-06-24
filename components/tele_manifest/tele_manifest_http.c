#include "tele_manifest_internal.h"

#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static bool is_redirect_status(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static esp_http_client_handle_t init_client(const char *url)
{
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = CONFIG_TELE_MANIFEST_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .max_redirection_count = CONFIG_TELE_MANIFEST_MAX_REDIRECTS,
        .buffer_size = CONFIG_TELE_MANIFEST_DOWNLOAD_BUFFER_SIZE,
        .buffer_size_tx = CONFIG_TELE_MANIFEST_DOWNLOAD_BUFFER_SIZE,
    };

    return esp_http_client_init(&config);
}

esp_err_t tele_manifest_http_get_text(const char *url,
                                      char *out_text,
                                      size_t out_size,
                                      int *out_status)
{
    if (!url || url[0] == '\0' || !out_text || out_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_status) {
        *out_status = 0;
    }

    esp_http_client_handle_t client = init_client(url);
    if (!client) {
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    int status = 0;
    int64_t content_length = 0;

    for (int redirect_count = 0; redirect_count <= CONFIG_TELE_MANIFEST_MAX_REDIRECTS; redirect_count++) {
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            goto cleanup;
        }

        content_length = esp_http_client_fetch_headers(client);
        if (content_length < 0) {
            err = ESP_FAIL;
            goto cleanup;
        }

        status = esp_http_client_get_status_code(client);
        if (!is_redirect_status(status)) {
            break;
        }

        err = esp_http_client_set_redirection(client);
        esp_http_client_close(client);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }

    if (out_status) {
        *out_status = status;
    }

    if (status != 200) {
        err = ESP_FAIL;
        goto cleanup;
    }
    if (content_length >= (int64_t)out_size) {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    int read_len = esp_http_client_read_response(client, out_text, out_size - 1);
    if (read_len < 0) {
        err = ESP_FAIL;
        goto cleanup;
    }
    out_text[read_len] = '\0';

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t tele_manifest_http_download(const char *url,
                                      size_t expected_size,
                                      tele_manifest_download_chunk_cb_t on_chunk,
                                      void *ctx,
                                      int *out_status,
                                      size_t *out_received)
{
    if (!url || url[0] == '\0' || expected_size == 0 || !on_chunk) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_status) {
        *out_status = 0;
    }
    if (out_received) {
        *out_received = 0;
    }

    esp_http_client_handle_t client = init_client(url);
    if (!client) {
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    int status = 0;
    int64_t content_length = 0;
    uint8_t *buffer = NULL;
    size_t received = 0;

    for (int redirect_count = 0; redirect_count <= CONFIG_TELE_MANIFEST_MAX_REDIRECTS; redirect_count++) {
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            goto cleanup;
        }

        content_length = esp_http_client_fetch_headers(client);
        if (content_length < 0) {
            err = ESP_FAIL;
            goto cleanup;
        }

        status = esp_http_client_get_status_code(client);
        if (!is_redirect_status(status)) {
            break;
        }

        err = esp_http_client_set_redirection(client);
        esp_http_client_close(client);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }

    if (out_status) {
        *out_status = status;
    }

    if (status != 200) {
        err = ESP_FAIL;
        goto cleanup;
    }
    if (content_length > 0 && (size_t)content_length != expected_size) {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    buffer = malloc(CONFIG_TELE_MANIFEST_DOWNLOAD_BUFFER_SIZE);
    if (!buffer) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    while (received < expected_size) {
        size_t remaining = expected_size - received;
        int to_read = remaining < CONFIG_TELE_MANIFEST_DOWNLOAD_BUFFER_SIZE ?
                      (int)remaining :
                      CONFIG_TELE_MANIFEST_DOWNLOAD_BUFFER_SIZE;
        int read_len = esp_http_client_read(client, (char *)buffer, to_read);

        if (read_len < 0) {
            err = ESP_FAIL;
            goto cleanup;
        }
        if (read_len == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if ((size_t)read_len > expected_size - received) {
            err = ESP_ERR_INVALID_SIZE;
            goto cleanup;
        }

        received += (size_t)read_len;
        err = on_chunk(buffer, (size_t)read_len, received, expected_size, ctx);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }

    if (received != expected_size) {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

cleanup:
    if (out_received) {
        *out_received = received;
    }
    free(buffer);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}
