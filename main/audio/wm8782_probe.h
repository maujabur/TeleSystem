#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool present;
    size_t bytes_read;
    int16_t min_sample;
    int16_t max_sample;
} wm8782_probe_result_t;

esp_err_t wm8782_probe(wm8782_probe_result_t *result);

#ifdef __cplusplus
}
#endif
