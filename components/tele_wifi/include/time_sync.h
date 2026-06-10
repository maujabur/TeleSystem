#ifndef TIME_SYNC_H
#define TIME_SYNC_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t time_sync_init(void);
void time_sync_on_network_connected(void);
void time_sync_on_network_disconnected(void);
bool time_sync_is_synchronized(void);
const char *time_sync_timezone(void);
bool time_sync_format_local_now(char *buffer, size_t buffer_len);
bool time_sync_format_utc_now(char *buffer, size_t buffer_len);

#ifdef __cplusplus
}
#endif

#endif
