#ifndef ACR_TRIGGER_OUTPUT_H
#define ACR_TRIGGER_OUTPUT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enabled;
    int gpio;
    int active_level;
    uint32_t pulse_ms;
} acr_trigger_output_config_t;

typedef struct {
    acr_trigger_output_config_t config;
    int64_t last_pulse_at_ms;
    esp_err_t last_error;
} acr_trigger_output_status_t;

esp_err_t acr_trigger_output_init(void);
esp_err_t acr_trigger_output_get_status(acr_trigger_output_status_t *out);
esp_err_t acr_trigger_output_save_config(const acr_trigger_output_config_t *config);
esp_err_t acr_trigger_output_pulse(void);

#ifdef __cplusplus
}
#endif

#endif
