#include "acr_runtime_status.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

static acr_runtime_status_t s_status;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;

void acr_runtime_status_clear(void)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.retry_pending = false;
    s_status.retry_at_ms = 0;
    s_status.last_error = ESP_OK;
    s_status.message[0] = '\0';
    s_status.uploaded_name[0] = '\0';
    portEXIT_CRITICAL(&s_status_lock);
}

void acr_runtime_status_set_state(acr_runtime_state_t state, const char *message)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.state = state;
    s_status.retry_pending = state == ACR_RUNTIME_STATE_RETRY_WAIT;
    s_status.last_error = state == ACR_RUNTIME_STATE_ERROR ? s_status.last_error : ESP_OK;
    if (state != ACR_RUNTIME_STATE_SILENCE_DISCARDED) {
        s_status.audio_last_silence_discarded = false;
    }
    s_status.message[0] = '\0';
    if (message) {
        snprintf(s_status.message, sizeof(s_status.message), "%s", message);
    }
    portEXIT_CRITICAL(&s_status_lock);
}

void acr_runtime_status_set_message(const char *message)
{
    acr_runtime_status_set_message_for_file(message, NULL);
}

void acr_runtime_status_set_message_for_file(const char *message, const char *uploaded_name)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.retry_pending = false;
    s_status.retry_at_ms = 0;
    s_status.last_error = ESP_OK;
    s_status.message[0] = '\0';
    s_status.uploaded_name[0] = '\0';
    if (message) {
        snprintf(s_status.message, sizeof(s_status.message), "%s", message);
    }
    if (uploaded_name) {
        snprintf(s_status.uploaded_name, sizeof(s_status.uploaded_name), "%s", uploaded_name);
    }
    portEXIT_CRITICAL(&s_status_lock);
}

void acr_runtime_status_set_retry(esp_err_t err, int64_t retry_at_ms, const char *message)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.retry_pending = true;
    s_status.retry_at_ms = retry_at_ms;
    s_status.last_error = err;
    s_status.state = ACR_RUNTIME_STATE_RETRY_WAIT;
    s_status.consecutive_errors++;
    s_status.message[0] = '\0';
    s_status.uploaded_name[0] = '\0';
    if (message) {
        snprintf(s_status.message, sizeof(s_status.message), "%s", message);
    }
    portEXIT_CRITICAL(&s_status_lock);
}

void acr_runtime_status_set_timings(int64_t capture_ms,
                                    int64_t upload_ms,
                                    int64_t upload_connect_ms,
                                    int64_t upload_write_ms,
                                    int64_t upload_response_ms,
                                    int64_t response_wait_ms,
                                    int64_t total_cycle_ms)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.capture_ms = capture_ms;
    s_status.upload_ms = upload_ms;
    s_status.upload_connect_ms = upload_connect_ms;
    s_status.upload_write_ms = upload_write_ms;
    s_status.upload_response_ms = upload_response_ms;
    s_status.response_wait_ms = response_wait_ms;
    s_status.total_cycle_ms = total_cycle_ms;
    portEXIT_CRITICAL(&s_status_lock);
}

void acr_runtime_status_set_audio_capture(size_t bytes_written,
                                          uint32_t active_ms,
                                          double rms,
                                          double peak_percent,
                                          bool clipped,
                                          bool silence_discarded)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.audio_last_capture_size = bytes_written;
    s_status.audio_last_active_ms = active_ms;
    s_status.audio_last_rms = rms;
    s_status.audio_last_peak_percent = peak_percent;
    s_status.audio_last_clipped = clipped;
    s_status.audio_last_clipped_detected = clipped;
    s_status.audio_last_silence_discarded = silence_discarded;
    s_status.audio_last_silence_detected = silence_discarded;
    portEXIT_CRITICAL(&s_status_lock);
}

void acr_runtime_status_increment_acr_submitted(void)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.acr_submitted_count++;
    portEXIT_CRITICAL(&s_status_lock);
}

void acr_runtime_status_increment_silence_discarded(void)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.silence_discarded_count++;
    portEXIT_CRITICAL(&s_status_lock);
}

void acr_runtime_status_increment_acr_error(void)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.acr_error_count++;
    portEXIT_CRITICAL(&s_status_lock);
}

void acr_runtime_status_set_last_result(bool trigger,
                                        double ai_probability,
                                        const char *prediction,
                                        const char *uploaded_name,
                                        int64_t timestamp_ms)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.last_trigger = trigger;
    s_status.last_ai_probability = ai_probability;
    s_status.last_result_at_ms = timestamp_ms;
    s_status.consecutive_errors = 0;
    s_status.last_prediction[0] = '\0';
    s_status.last_uploaded_name[0] = '\0';
    if (prediction) {
        snprintf(s_status.last_prediction, sizeof(s_status.last_prediction), "%s", prediction);
    }
    if (uploaded_name) {
        snprintf(s_status.last_uploaded_name, sizeof(s_status.last_uploaded_name), "%s", uploaded_name);
    }
    portEXIT_CRITICAL(&s_status_lock);
}

void acr_runtime_status_get(acr_runtime_status_t *out)
{
    if (!out) {
        return;
    }

    portENTER_CRITICAL(&s_status_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_status_lock);
}

acr_runtime_state_t acr_runtime_status_get_state(void)
{
    acr_runtime_state_t state;

    portENTER_CRITICAL(&s_status_lock);
    state = s_status.state;
    portEXIT_CRITICAL(&s_status_lock);

    return state;
}

const char *acr_runtime_status_state_name(acr_runtime_state_t state)
{
    switch (state) {
    case ACR_RUNTIME_STATE_IDLE:
        return "idle";
    case ACR_RUNTIME_STATE_CAPTURING:
        return "capturing";
    case ACR_RUNTIME_STATE_SILENCE_DISCARDED:
        return "silence_discarded";
    case ACR_RUNTIME_STATE_UPLOADING:
        return "uploading";
    case ACR_RUNTIME_STATE_WAITING_ACR:
        return "waiting_acr";
    case ACR_RUNTIME_STATE_RESULT_HUMAN:
        return "result_human";
    case ACR_RUNTIME_STATE_RESULT_AI:
        return "result_ai";
    case ACR_RUNTIME_STATE_ERROR:
        return "error";
    case ACR_RUNTIME_STATE_RETRY_WAIT:
        return "retry_wait";
    default:
        return "unknown";
    }
}
