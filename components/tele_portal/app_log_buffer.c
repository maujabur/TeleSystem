#include "app_log_buffer.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define APP_LOG_BUFFER_SIZE 8192
#define APP_LOG_LINE_BUFFER_SIZE 512

#ifndef CONFIG_APP_LOG_BUFFER_CAPTURE_DEBUG
#define CONFIG_APP_LOG_BUFFER_CAPTURE_DEBUG 0
#endif

static char s_log_buffer[APP_LOG_BUFFER_SIZE];
static size_t s_log_write_index;
static size_t s_log_length;
static portMUX_TYPE s_log_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_previous_vprintf;

static bool should_capture_log_line(const char *line)
{
    if (!line) {
        return false;
    }

    if (CONFIG_APP_LOG_BUFFER_CAPTURE_DEBUG) {
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

static int app_log_vprintf(const char *format, va_list args)
{
    char line[APP_LOG_LINE_BUFFER_SIZE];
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

void app_log_buffer_init(void)
{
    static bool initialized;

    if (initialized) {
        return;
    }

    memset(s_log_buffer, 0, sizeof(s_log_buffer));
    s_log_write_index = 0;
    s_log_length = 0;
    s_previous_vprintf = esp_log_set_vprintf(app_log_vprintf);
    initialized = true;
}

size_t app_log_buffer_get_snapshot(char *out, size_t out_size)
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
