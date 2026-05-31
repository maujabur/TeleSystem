#ifndef STORAGE_H
#define STORAGE_H

#include "esp_err.h"

esp_err_t storage_mount(void);
int storage_file_exists(const char *path);
long storage_file_size(const char *path);
void storage_log_file_info(const char *path);

#endif
