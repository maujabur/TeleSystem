#ifndef ACR_ANALYSIS_CONTROL_H
#define ACR_ANALYSIS_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#include "acr_client.h"

typedef enum {
    ACR_ANALYSIS_TRIGGER_NONE = 0,
    ACR_ANALYSIS_TRIGGER_MANUAL,
    ACR_ANALYSIS_TRIGGER_AUTO,
} acr_analysis_trigger_t;

typedef struct {
    bool automatic_enabled;
    uint32_t automatic_interval_ms;
    uint32_t capture_duration_seconds;
    double digital_gain;
    uint32_t silence_threshold_rms;
    uint32_t silence_hysteresis_rms;
    uint32_t min_active_ms;
    acr_trigger_mode_t trigger_mode;
    double ai_probability_threshold;
} acr_analysis_control_config_t;

void acr_analysis_control_init(void);
void acr_analysis_control_request(void);
acr_analysis_trigger_t acr_analysis_control_wait(TickType_t timeout_ticks);
void acr_analysis_control_set_last_cycle_ms(int64_t timestamp_ms);
esp_err_t acr_analysis_control_get_config(acr_analysis_control_config_t *out);
esp_err_t acr_analysis_control_save_config(const acr_analysis_control_config_t *config);

#endif
