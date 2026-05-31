#ifndef ACR_RUNTIME_STATUS_H
#define ACR_RUNTIME_STATUS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ACR_RUNTIME_STATUS_TEXT_SIZE 96
#define ACR_RUNTIME_STATUS_UPLOAD_NAME_SIZE 128
#define ACR_RUNTIME_STATUS_PREDICTION_SIZE 32

typedef enum {
    ACR_RUNTIME_STATE_IDLE = 0,
    ACR_RUNTIME_STATE_CAPTURING,
    ACR_RUNTIME_STATE_SILENCE_DISCARDED,
    ACR_RUNTIME_STATE_UPLOADING,
    ACR_RUNTIME_STATE_WAITING_ACR,
    ACR_RUNTIME_STATE_RESULT_HUMAN,
    ACR_RUNTIME_STATE_RESULT_AI,
    ACR_RUNTIME_STATE_ERROR,
    ACR_RUNTIME_STATE_RETRY_WAIT,
} acr_runtime_state_t;

typedef struct {
    bool retry_pending;
    bool last_trigger;
    uint32_t consecutive_errors;
    uint32_t acr_submitted_count;
    uint32_t silence_discarded_count;
    uint32_t acr_error_count;
    int64_t retry_at_ms;
    int64_t last_result_at_ms;
    int64_t capture_ms;
    int64_t upload_ms;
    int64_t upload_connect_ms;
    int64_t upload_write_ms;
    int64_t upload_response_ms;
    int64_t response_wait_ms;
    int64_t total_cycle_ms;
    size_t audio_last_capture_size;
    uint32_t audio_last_active_ms;
    esp_err_t last_error;
    acr_runtime_state_t state;
    double audio_last_rms;
    double audio_last_peak_percent;
    double last_ai_probability;
    bool audio_last_clipped;
    bool audio_last_clipped_detected;
    bool audio_last_silence_discarded;
    bool audio_last_silence_detected;
    char message[ACR_RUNTIME_STATUS_TEXT_SIZE];
    char uploaded_name[ACR_RUNTIME_STATUS_UPLOAD_NAME_SIZE];
    char last_prediction[ACR_RUNTIME_STATUS_PREDICTION_SIZE];
    char last_uploaded_name[ACR_RUNTIME_STATUS_UPLOAD_NAME_SIZE];
} acr_runtime_status_t;

void acr_runtime_status_clear(void);
void acr_runtime_status_set_state(acr_runtime_state_t state, const char *message);
void acr_runtime_status_set_message(const char *message);
void acr_runtime_status_set_message_for_file(const char *message, const char *uploaded_name);
void acr_runtime_status_set_retry(esp_err_t err, int64_t retry_at_ms, const char *message);
void acr_runtime_status_set_timings(int64_t capture_ms,
                                    int64_t upload_ms,
                                    int64_t upload_connect_ms,
                                    int64_t upload_write_ms,
                                    int64_t upload_response_ms,
                                    int64_t response_wait_ms,
                                    int64_t total_cycle_ms);
void acr_runtime_status_set_audio_capture(size_t bytes_written,
                                          uint32_t active_ms,
                                          double rms,
                                          double peak_percent,
                                          bool clipped,
                                          bool silence_discarded);
void acr_runtime_status_increment_acr_submitted(void);
void acr_runtime_status_increment_silence_discarded(void);
void acr_runtime_status_increment_acr_error(void);
void acr_runtime_status_set_last_result(bool trigger,
                                        double ai_probability,
                                        const char *prediction,
                                        const char *uploaded_name,
                                        int64_t timestamp_ms);
void acr_runtime_status_get(acr_runtime_status_t *out);
acr_runtime_state_t acr_runtime_status_get_state(void);
const char *acr_runtime_status_state_name(acr_runtime_state_t state);

#ifdef __cplusplus
}
#endif

#endif
