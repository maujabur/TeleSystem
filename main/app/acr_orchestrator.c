#include "acr_orchestrator.h"

#include <stdbool.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "acr_analysis_control.h"
#include "acr_client.h"
#include "acr_config_store.h"
#include "acr_runtime_status.h"
#include "acr_trigger_output.h"
#include "audio_capture.h"
#include "power_good.h"
#include "status_led.h"
#include "vbat_monitor.h"
#include "wifi_manager.h"

static const char *TAG = "acr-orchestrator";

// Keep large ACR config buffers out of the task stack.
static acr_config_t s_acr_config;

static void perform_low_battery_shutdown(const char *context)
{
    ESP_LOGE(TAG,
             "VBAT critica [%s] — desligando perifericos e entrando em deep sleep",
             context);
    status_led_set_state(STATUS_LED_STATE_LOW_BATTERY);
    /* Aguarda o LED piscar ao menos uma vez para indicacao visual */
    vTaskDelay(pdMS_TO_TICKS(1200));
    power_good_set(false);
    esp_wifi_stop();
    esp_deep_sleep_start();
}

#define WAV_PATH AUDIO_CAPTURE_DEFAULT_PATH
#define WIFI_READY_RETRY_DELAY_MS 1000
#define ACR_WIFI_RECOVERY_CONSECUTIVE_ERRORS 3

#ifndef CONFIG_ACR_SUBMIT_RETRY_DELAY_MS
#define CONFIG_ACR_SUBMIT_RETRY_DELAY_MS 60000
#endif

static bool transition_allowed(acr_runtime_state_t from, acr_runtime_state_t to)
{
    if (from == to) {
        return true;
    }

    switch (from) {
    case ACR_RUNTIME_STATE_IDLE:
    case ACR_RUNTIME_STATE_SILENCE_DISCARDED:
    case ACR_RUNTIME_STATE_RESULT_HUMAN:
    case ACR_RUNTIME_STATE_RESULT_AI:
    case ACR_RUNTIME_STATE_ERROR:
    case ACR_RUNTIME_STATE_RETRY_WAIT:
        return to == ACR_RUNTIME_STATE_CAPTURING || to == ACR_RUNTIME_STATE_IDLE;
    case ACR_RUNTIME_STATE_CAPTURING:
        return to == ACR_RUNTIME_STATE_UPLOADING ||
               to == ACR_RUNTIME_STATE_WAITING_ACR ||
               to == ACR_RUNTIME_STATE_SILENCE_DISCARDED ||
               to == ACR_RUNTIME_STATE_ERROR;
    case ACR_RUNTIME_STATE_UPLOADING:
        return to == ACR_RUNTIME_STATE_WAITING_ACR ||
               to == ACR_RUNTIME_STATE_RESULT_HUMAN ||
               to == ACR_RUNTIME_STATE_RESULT_AI ||
               to == ACR_RUNTIME_STATE_ERROR;
    case ACR_RUNTIME_STATE_WAITING_ACR:
        return to == ACR_RUNTIME_STATE_RESULT_HUMAN ||
               to == ACR_RUNTIME_STATE_RESULT_AI ||
               to == ACR_RUNTIME_STATE_ERROR;
    default:
        return false;
    }
}

static void update_led_for_state(acr_runtime_state_t state)
{
    switch (state) {
    case ACR_RUNTIME_STATE_UPLOADING:
        status_led_set_state(STATUS_LED_STATE_ACR_UPLOADING);
        break;
    case ACR_RUNTIME_STATE_WAITING_ACR:
        status_led_set_state(STATUS_LED_STATE_ACR_WAITING_RESULT);
        break;
    case ACR_RUNTIME_STATE_RESULT_HUMAN:
        status_led_set_state(STATUS_LED_STATE_ACR_RESULT_HUMAN);
        break;
    case ACR_RUNTIME_STATE_RESULT_AI:
        status_led_set_state(STATUS_LED_STATE_ACR_RESULT_AI);
        break;
    case ACR_RUNTIME_STATE_ERROR:
        status_led_set_state(STATUS_LED_STATE_ERROR);
        break;
    default:
        break;
    }
}

static void orchestrator_transition_to(acr_runtime_state_t next_state,
                                       const char *message,
                                       const char *uploaded_name)
{
    acr_runtime_state_t current_state = acr_runtime_status_get_state();

    if (!transition_allowed(current_state, next_state)) {
        ESP_LOGW(TAG,
                 "Transicao de estado ignorada: %s -> %s",
                 acr_runtime_status_state_name(current_state),
                 acr_runtime_status_state_name(next_state));
        return;
    }

    acr_runtime_status_set_state(next_state, message);
    if (uploaded_name && uploaded_name[0] != '\0') {
        acr_runtime_status_set_message_for_file(message, uploaded_name);
    }
    update_led_for_state(next_state);
}

static void acr_client_event_handler(void *arg,
                                     esp_event_base_t event_base,
                                     int32_t event_id,
                                     void *event_data)
{
    (void)arg;
    (void)event_base;

    const char *uploaded_name = event_data ? (const char *)event_data : NULL;

    switch (event_id) {
    case ACR_CLIENT_EVENT_UPLOAD_STARTED:
        orchestrator_transition_to(ACR_RUNTIME_STATE_UPLOADING,
                                   "Enviando arquivo para ACRCloud",
                                   uploaded_name);
        break;
    case ACR_CLIENT_EVENT_WAITING_RESULT:
        orchestrator_transition_to(ACR_RUNTIME_STATE_WAITING_ACR,
                                   "Aguardando resultado da ACRCloud",
                                   uploaded_name);
        break;
    case ACR_CLIENT_EVENT_RESULT_HUMAN:
        orchestrator_transition_to(ACR_RUNTIME_STATE_RESULT_HUMAN,
                                   "Resultado ACR: humano",
                                   uploaded_name);
        break;
    case ACR_CLIENT_EVENT_RESULT_AI:
        orchestrator_transition_to(ACR_RUNTIME_STATE_RESULT_AI,
                                   "Resultado ACR: IA",
                                   uploaded_name);
        break;
    case ACR_CLIENT_EVENT_FAILED:
        orchestrator_transition_to(ACR_RUNTIME_STATE_ERROR,
                                   "Falha no envio ou consulta ACR",
                                   uploaded_name);
        break;
    default:
        break;
    }
}

static esp_err_t wait_until_wifi_ready(void)
{
    wifi_manager_status_t wifi_status;

    while (true) {
        esp_err_t err = wifi_manager_wait_until_ready(pdMS_TO_TICKS(WIFI_READY_RETRY_DELAY_MS));
        if (err == ESP_OK) {
            return ESP_OK;
        }

        if (err == ESP_ERR_WIFI_NOT_CONNECT) {
            ESP_ERROR_CHECK(wifi_manager_get_status(&wifi_status));
            ESP_LOGW(TAG,
                     "Wi-Fi wait | reason=provisioning | state=%d | ssid=%s",
                     (int)wifi_status.state,
                     wifi_status.ssid);
            vTaskDelay(pdMS_TO_TICKS(WIFI_READY_RETRY_DELAY_MS));
            continue;
        }

        if (err != ESP_ERR_TIMEOUT) {
            return err;
        }
    }
}

static void maybe_recover_wifi_after_retry(void)
{
    acr_runtime_status_t status = {0};

    acr_runtime_status_get(&status);
    if (status.consecutive_errors < ACR_WIFI_RECOVERY_CONSECUTIVE_ERRORS ||
        status.consecutive_errors % ACR_WIFI_RECOVERY_CONSECUTIVE_ERRORS != 0) {
        return;
    }

    ESP_LOGW(TAG,
             "Wi-Fi recovery | reason=acr_network_errors | consecutive_errors=%u",
             (unsigned)status.consecutive_errors);
    esp_err_t wifi_err = wifi_manager_reconnect_sta();
    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi recovery | result=failed | error=%s", esp_err_to_name(wifi_err));
    }
}

static void schedule_retry(esp_err_t err, const char *message, bool recover_wifi)
{
    int64_t retry_at_ms = esp_log_timestamp() + CONFIG_ACR_SUBMIT_RETRY_DELAY_MS;

    acr_runtime_status_set_retry(err, retry_at_ms, message);
    if (recover_wifi) {
        maybe_recover_wifi_after_retry();
    }
    acr_analysis_control_set_last_cycle_ms(retry_at_ms);
}

static bool state_accepts_force_cycle(acr_runtime_state_t state)
{
    switch (state) {
    case ACR_RUNTIME_STATE_IDLE:
    case ACR_RUNTIME_STATE_SILENCE_DISCARDED:
    case ACR_RUNTIME_STATE_RESULT_HUMAN:
    case ACR_RUNTIME_STATE_RESULT_AI:
    case ACR_RUNTIME_STATE_ERROR:
    case ACR_RUNTIME_STATE_RETRY_WAIT:
        return true;
    case ACR_RUNTIME_STATE_CAPTURING:
    case ACR_RUNTIME_STATE_UPLOADING:
    case ACR_RUNTIME_STATE_WAITING_ACR:
    default:
        return false;
    }
}

esp_err_t acr_orchestrator_force_cycle(void)
{
    acr_runtime_state_t state = acr_runtime_status_get_state();

    if (!state_accepts_force_cycle(state)) {
        ESP_LOGW(TAG,
                 "Force cycle | result=rejected | state=%s",
                 acr_runtime_status_state_name(state));
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG,
             "Force cycle | result=accepted | state=%s",
             acr_runtime_status_state_name(state));
    acr_analysis_control_request();
    return ESP_OK;
}

static void run_cycle(acr_analysis_trigger_t analysis_trigger)
{
    esp_err_t err;
    esp_err_t led_err = ESP_OK;
    int64_t cycle_start_ms = esp_log_timestamp();
    int64_t capture_start_ms = 0;
    int64_t capture_end_ms = 0;
    int64_t acr_end_ms = 0;
    acr_analysis_control_config_t analysis_config = {0};

    ESP_LOGI(TAG,
             "ACR cycle | trigger=%s | status=requested",
             analysis_trigger == ACR_ANALYSIS_TRIGGER_AUTO ? "auto" : "manual");

    err = acr_analysis_control_get_config(&analysis_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao carregar controle ACR: %s", esp_err_to_name(err));
        schedule_retry(err, "Falha ao carregar controle ACR", false);
        return;
    }

    err = wait_until_wifi_ready();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi indisponivel para analise ACR: %s", esp_err_to_name(err));
        orchestrator_transition_to(ACR_RUNTIME_STATE_ERROR,
                                   "Wi-Fi indisponivel para analise ACR",
                                   NULL);
        schedule_retry(err, "Wi-Fi indisponivel para analise ACR", true);
        return;
    }

    audio_capture_result_t capture_result = {0};
    audio_capture_buffer_t capture_buffer = {0};
    orchestrator_transition_to(ACR_RUNTIME_STATE_CAPTURING, "Capturando audio", NULL);
    led_err = status_led_set_capture_overlay(true);
    if (led_err != ESP_OK) {
        ESP_LOGW(TAG, "Nao foi possivel ativar overlay de captura no LED: %s", esp_err_to_name(led_err));
    }
    capture_start_ms = esp_log_timestamp();
    err = audio_capture_record_pcm_to_buffer((int)analysis_config.capture_duration_seconds,
                                             (int)(analysis_config.digital_gain * 256.0 + 0.5),
                                             (int)analysis_config.silence_threshold_rms,
                                             (int)analysis_config.silence_hysteresis_rms,
                                             &capture_result,
                                             &capture_buffer);
    led_err = status_led_set_capture_overlay(false);
    if (led_err != ESP_OK) {
        ESP_LOGW(TAG, "Nao foi possivel desativar overlay de captura no LED: %s", esp_err_to_name(led_err));
    }
    capture_end_ms = esp_log_timestamp();
    acr_runtime_status_set_timings(capture_end_ms - capture_start_ms,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                                   capture_end_ms - cycle_start_ms);
    vbat_monitor_maybe_measure(VBAT_MONITOR_MOMENT_AFTER_AUDIO_CAPTURE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha na captura de audio: %s", esp_err_to_name(err));
        orchestrator_transition_to(ACR_RUNTIME_STATE_ERROR,
                                   "Falha na captura de audio",
                                   NULL);
        schedule_retry(err, "Falha na captura de audio", false);
        return;
    }

    double peak_percent = ((double)capture_result.peak_sample * 100.0) / 32767.0;
    bool silence_discarded = analysis_config.min_active_ms > 0 &&
        capture_result.active_ms < analysis_config.min_active_ms;
    acr_runtime_status_set_audio_capture(capture_result.bytes_written,
                                         capture_result.active_ms,
                                         capture_result.rms,
                                         peak_percent,
                                         capture_result.clipped,
                                         silence_discarded);
    ESP_LOGI(TAG,
             "Audio capture | pcm_bytes=%u | active=%ums | rms=%.1f | gain=%.2fx | silence_threshold=%u | hysteresis=%u | min_active=%ums | peak=%d | clipped=%s",
             (unsigned)capture_result.bytes_written,
             (unsigned)capture_result.active_ms,
             capture_result.rms,
             (double)capture_result.gain_q8 / 256.0,
             (unsigned)analysis_config.silence_threshold_rms,
             (unsigned)analysis_config.silence_hysteresis_rms,
             (unsigned)analysis_config.min_active_ms,
             (int)capture_result.peak_sample,
             capture_result.clipped ? "sim" : "nao");
    if (silence_discarded) {
        ESP_LOGI(TAG,
                 "Audio silence | decision=discard | active=%ums | min_active=%ums",
                 (unsigned)capture_result.active_ms,
                 (unsigned)analysis_config.min_active_ms);
        orchestrator_transition_to(ACR_RUNTIME_STATE_SILENCE_DISCARDED,
                                   "Silencio descartado",
                                   NULL);
        acr_runtime_status_increment_silence_discarded();
        acr_analysis_control_set_last_cycle_ms(0);
        audio_capture_buffer_free(&capture_buffer);
        return;
    }

    err = acr_config_store_load(&s_acr_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao carregar configuracao ACR: %s", esp_err_to_name(err));
        orchestrator_transition_to(ACR_RUNTIME_STATE_ERROR,
                                   "Falha ao carregar configuracao ACR",
                                   NULL);
        schedule_retry(err, "Falha ao carregar configuracao ACR", false);
        audio_capture_buffer_free(&capture_buffer);
        return;
    }

    acr_runtime_status_increment_acr_submitted();

    wifi_manager_status_t wifi_status = {0};
    if (wifi_manager_get_status(&wifi_status) == ESP_OK) {
        ESP_LOGI(TAG,
                 "Wi-Fi before ACR upload | ssid=%s | rssi=%ddBm | ip=%s",
                 wifi_status.ssid,
                 wifi_status.rssi,
                 wifi_status.ip);
    }

    acr_client_result_t acr_result = {0};
    acr_runtime_status_clear();
    err = acr_client_submit_pcm_wav_and_wait_result(&s_acr_config,
                                                    capture_buffer.pcm_data,
                                                    capture_buffer.pcm_size,
                                                    capture_buffer.sample_rate_hz,
                                                    capture_buffer.channels,
                                                    capture_buffer.bits_per_sample,
                                                    &acr_result);
    audio_capture_buffer_free(&capture_buffer);
    acr_end_ms = esp_log_timestamp();
    acr_runtime_status_set_timings(capture_end_ms - capture_start_ms,
                                   acr_result.upload_ms,
                                   acr_result.upload_connect_ms,
                                   acr_result.upload_write_ms,
                                   acr_result.upload_response_ms,
                                   acr_result.response_wait_ms,
                                   acr_end_ms - cycle_start_ms);
    if (err == ESP_OK) {
        acr_runtime_status_set_last_result(acr_result.trigger,
                                           acr_result.ai_probability,
                                           acr_result.prediction,
                                           acr_result.uploaded_name,
                                           acr_end_ms);
        orchestrator_transition_to(acr_result.trigger ?
                                       ACR_RUNTIME_STATE_RESULT_AI :
                                       ACR_RUNTIME_STATE_RESULT_HUMAN,
                                   acr_result.trigger ? "Resultado ACR: IA" : "Resultado ACR: humano",
                                   acr_result.uploaded_name);
        ESP_LOGI(TAG,
                 "ACR result | decision=%d | trigger=%s | prediction=%s | probability=%.2f%% | upload=%s | capture=%lldms | upload_time=%lldms | upload_connect=%lldms | upload_write=%lldms | upload_response=%lldms | response_time=%lldms | total=%lldms",
                 (int)acr_result.decision,
                 acr_result.trigger ? "sim" : "nao",
                 acr_result.prediction,
                 acr_result.ai_probability,
                 acr_result.uploaded_name,
                 (long long)(capture_end_ms - capture_start_ms),
                 (long long)acr_result.upload_ms,
                 (long long)acr_result.upload_connect_ms,
                 (long long)acr_result.upload_write_ms,
                 (long long)acr_result.upload_response_ms,
                 (long long)acr_result.response_wait_ms,
                 (long long)(acr_end_ms - cycle_start_ms));
        if (acr_result.trigger) {
            acr_trigger_output_pulse();
        }
        acr_analysis_control_set_last_cycle_ms(0);
        return;
    }

    ESP_LOGE(TAG, "ACR error | stage=submit_or_result | error=%s", esp_err_to_name(err));
    acr_runtime_status_increment_acr_error();
    orchestrator_transition_to(ACR_RUNTIME_STATE_ERROR,
                               "Falha no envio ou consulta ACR",
                               NULL);
    schedule_retry(err, "Falha no envio ou consulta ACR", true);
}

void acr_orchestrator_run(void)
{
    ESP_ERROR_CHECK(esp_event_handler_register(ACR_CLIENT_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               &acr_client_event_handler,
                                               NULL));

    orchestrator_transition_to(ACR_RUNTIME_STATE_IDLE,
                               "Aguardando solicitacao de analise",
                               NULL);

    while (true) {
        TickType_t wait_ticks = vbat_monitor_next_wait_timeout(portMAX_DELAY);
        acr_analysis_trigger_t analysis_trigger = acr_analysis_control_wait(wait_ticks);
        if (analysis_trigger == ACR_ANALYSIS_TRIGGER_NONE) {
            vbat_monitor_maybe_measure(VBAT_MONITOR_MOMENT_MAX_INTERVAL);
            if (vbat_monitor_check_shutdown()) {
                perform_low_battery_shutdown("idle");
            }
            continue;
        }

        run_cycle(analysis_trigger);
        if (vbat_monitor_check_shutdown()) {
            perform_low_battery_shutdown("pos-ciclo");
        }
    }
}
