#include "tele_portal_logs.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "tele_portal_core.h"

#define TELE_PORTAL_LOGS_BUFFER_SIZE 8192
#define TELE_PORTAL_LOGS_LINE_BUFFER_SIZE 512

#ifndef CONFIG_TELE_PORTAL_LOGS_ENABLE_ENDPOINT
#define CONFIG_TELE_PORTAL_LOGS_ENABLE_ENDPOINT 0
#endif

#ifndef CONFIG_TELE_PORTAL_LOGS_CAPTURE_DEBUG
#define CONFIG_TELE_PORTAL_LOGS_CAPTURE_DEBUG 0
#endif

static const char *TAG = "portal-logs";
static char s_log_buffer[TELE_PORTAL_LOGS_BUFFER_SIZE];
static size_t s_log_write_index;
static size_t s_log_length;
static portMUX_TYPE s_log_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_previous_vprintf;

bool tele_portal_logs_endpoint_enabled(void)
{
    return CONFIG_TELE_PORTAL_LOGS_ENABLE_ENDPOINT;
}

static bool should_capture_log_line(const char *line)
{
    if (!line) {
        return false;
    }

    if (CONFIG_TELE_PORTAL_LOGS_CAPTURE_DEBUG) {
        return true;
    }

    return !(strncmp(line, "D (", 3) == 0 || strncmp(line, "V (", 3) == 0);
}

static void append_log_text(const char *text)
{
    if (!text) {
        return;
    }

    portENTER_CRITICAL(&s_log_lock);
    for (size_t i = 0; text[i] != '\0'; ++i) {
        s_log_buffer[s_log_write_index] = text[i];
        s_log_write_index = (s_log_write_index + 1) % sizeof(s_log_buffer);
        if (s_log_length < sizeof(s_log_buffer)) {
            s_log_length++;
        }
    }
    portEXIT_CRITICAL(&s_log_lock);
}

static int portal_logs_vprintf(const char *format, va_list args)
{
    char line[TELE_PORTAL_LOGS_LINE_BUFFER_SIZE];
    va_list copy;
    int result = 0;

    va_copy(copy, args);
    vsnprintf(line, sizeof(line), format, copy);
    va_end(copy);

    if (should_capture_log_line(line)) {
        append_log_text(line);
    }

    if (s_previous_vprintf) {
        result = s_previous_vprintf(format, args);
    } else {
        result = vprintf(format, args);
    }

    return result;
}

void tele_portal_logs_init(void)
{
    static bool initialized;

    if (initialized) {
        return;
    }

    memset(s_log_buffer, 0, sizeof(s_log_buffer));
    s_log_write_index = 0;
    s_log_length = 0;
    s_previous_vprintf = esp_log_set_vprintf(portal_logs_vprintf);
    initialized = true;
}

size_t tele_portal_logs_get_snapshot(char *out, size_t out_size)
{
    size_t copy_len = 0;
    size_t start = 0;

    if (!out || out_size == 0) {
        return 0;
    }

    portENTER_CRITICAL(&s_log_lock);
    copy_len = s_log_length < out_size - 1 ? s_log_length : out_size - 1;
    start = (s_log_write_index + sizeof(s_log_buffer) - copy_len) % sizeof(s_log_buffer);
    for (size_t i = 0; i < copy_len; ++i) {
        out[i] = s_log_buffer[(start + i) % sizeof(s_log_buffer)];
    }
    portEXIT_CRITICAL(&s_log_lock);

    out[copy_len] = '\0';
    return copy_len;
}

static esp_err_t api_logs_get_handler(httpd_req_t *req)
{
    static char logs[TELE_PORTAL_LOGS_BUFFER_SIZE];

    if (!CONFIG_TELE_PORTAL_LOGS_ENABLE_ENDPOINT) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Endpoint indisponivel");
    }

    tele_portal_core_note_activity();
    tele_portal_logs_get_snapshot(logs, sizeof(logs));
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, logs);
}

esp_err_t tele_portal_logs_register_routes(httpd_handle_t server)
{
    httpd_uri_t api_logs = {
        .uri = "/api/logs",
        .method = HTTP_GET,
        .handler = api_logs_get_handler,
    };

    if (!CONFIG_TELE_PORTAL_LOGS_ENABLE_ENDPOINT) {
        return ESP_OK;
    }

    esp_err_t err = httpd_register_uri_handler(server, &api_logs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar rota /api/logs: %s", esp_err_to_name(err));
    }
    return err;
}
