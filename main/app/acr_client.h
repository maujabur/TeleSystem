#ifndef ACR_CLIENT_H
#define ACR_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_event.h"

#include "acr_config_store.h"

ESP_EVENT_DECLARE_BASE(ACR_CLIENT_EVENT);

typedef enum {
    ACR_CLIENT_EVENT_UPLOAD_STARTED = 1,
    ACR_CLIENT_EVENT_WAITING_RESULT,
    ACR_CLIENT_EVENT_RESULT_HUMAN,
    ACR_CLIENT_EVENT_RESULT_AI,
    ACR_CLIENT_EVENT_FAILED,
} acr_client_event_id_t;

typedef enum {
    ACR_DECISION_UNKNOWN = 0,
    ACR_DECISION_HUMAN,
    ACR_DECISION_AI,
} acr_decision_t;

typedef enum {
    ACR_TRIGGER_MODE_PREDICTION_ONLY = 0,
    ACR_TRIGGER_MODE_PROBABILITY_ONLY,
    ACR_TRIGGER_MODE_PREDICTION_OR_PROBABILITY,
    ACR_TRIGGER_MODE_PREDICTION_AND_PROBABILITY,
} acr_trigger_mode_t;

typedef struct {
    acr_decision_t decision;
    bool trigger;
    double ai_probability;
    int64_t upload_ms;
    int64_t upload_connect_ms;
    int64_t upload_write_ms;
    int64_t upload_response_ms;
    int64_t response_wait_ms;
    char prediction[32];
    char uploaded_name[128];
} acr_client_result_t;

esp_err_t acr_client_submit_and_wait_result(const acr_config_t *config,
                                            const char *path,
                                            acr_client_result_t *out);
esp_err_t acr_client_submit_pcm_wav_and_wait_result(const acr_config_t *config,
                                                    const void *pcm_data,
                                                    size_t pcm_size,
                                                    int sample_rate_hz,
                                                    int channels,
                                                    int bits_per_sample,
                                                    acr_client_result_t *out);
esp_err_t acr_client_submit_and_wait(const acr_config_t *config, const char *path);

#endif
