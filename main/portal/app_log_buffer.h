#ifndef APP_LOG_BUFFER_H
#define APP_LOG_BUFFER_H

#include <stddef.h>

void app_log_buffer_init(void);
size_t app_log_buffer_get_snapshot(char *out, size_t out_size);

#endif
