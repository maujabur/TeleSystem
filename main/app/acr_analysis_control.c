#include "acr_analysis_control.h"

#include "esp_log.h"
#include "freertos/event_groups.h"
#include "nvs.h"

#include "audio_capture.h"

#include <string.h>

#define ACR_ANALYSIS_REQUEST_BIT BIT0
#define ACR_ANALYSIS_CONFIG_CHANGED_BIT BIT1
#define ACR_ANALYSIS_NVS_NAMESPACE "acr_control"
#define ACR_ANALYSIS_NVS_AUTO_KEY "auto_enabled"
#define ACR_ANALYSIS_NVS_INTERVAL_KEY "auto_interval"
#define ACR_ANALYSIS_NVS_CAPTURE_DURATION_KEY "capture_sec"
#define ACR_ANALYSIS_NVS_DIGITAL_GAIN_KEY "gain_x100"
#define ACR_ANALYSIS_NVS_SILENCE_THRESHOLD_KEY "silence_rms"
#define ACR_ANALYSIS_NVS_SILENCE_HYSTERESIS_KEY "silence_hyst"
#define ACR_ANALYSIS_NVS_MIN_ACTIVE_MS_KEY "min_active_ms"
#define ACR_ANALYSIS_NVS_TRIGGER_MODE_KEY "trigger_mode"
#define ACR_ANALYSIS_NVS_THRESHOLD_X100_KEY "threshold_x100"

#ifndef CONFIG_ACR_AUTO_ENABLED
#define CONFIG_ACR_AUTO_ENABLED 0
#endif

#ifndef CONFIG_ACR_AUTO_INTERVAL_MS
#define CONFIG_ACR_AUTO_INTERVAL_MS 60000
#endif

#ifndef CONFIG_ACR_CAPTURE_DURATION_SECONDS
#define CONFIG_ACR_CAPTURE_DURATION_SECONDS AUDIO_CAPTURE_DEFAULT_SECONDS
#endif

#ifndef CONFIG_ACR_DIGITAL_GAIN_X100
#define CONFIG_ACR_DIGITAL_GAIN_X100 350
#endif

#ifndef CONFIG_ACR_SILENCE_THRESHOLD_RMS
#define CONFIG_ACR_SILENCE_THRESHOLD_RMS 170
#endif

#ifndef CONFIG_ACR_SILENCE_HYSTERESIS_RMS
#define CONFIG_ACR_SILENCE_HYSTERESIS_RMS 10
#endif

#ifndef CONFIG_ACR_MIN_ACTIVE_MS
#define CONFIG_ACR_MIN_ACTIVE_MS 2000
#endif

#ifndef CONFIG_ACR_TRIGGER_DEFAULT_MODE
#define CONFIG_ACR_TRIGGER_DEFAULT_MODE ACR_TRIGGER_MODE_PREDICTION_ONLY
#endif

#ifndef CONFIG_ACR_TRIGGER_DEFAULT_AI_PROBABILITY_THRESHOLD_X100
#define CONFIG_ACR_TRIGGER_DEFAULT_AI_PROBABILITY_THRESHOLD_X100 7000
#endif

static const char *TAG = "acr-control";

static EventGroupHandle_t s_event_group;
static acr_analysis_control_config_t s_config = {
    .automatic_enabled = CONFIG_ACR_AUTO_ENABLED,
    .automatic_interval_ms = CONFIG_ACR_AUTO_INTERVAL_MS,
    .capture_duration_seconds = CONFIG_ACR_CAPTURE_DURATION_SECONDS,
    .digital_gain = CONFIG_ACR_DIGITAL_GAIN_X100 / 100.0,
    .silence_threshold_rms = CONFIG_ACR_SILENCE_THRESHOLD_RMS,
    .silence_hysteresis_rms = CONFIG_ACR_SILENCE_HYSTERESIS_RMS,
    .min_active_ms = CONFIG_ACR_MIN_ACTIVE_MS,
    .trigger_mode = CONFIG_ACR_TRIGGER_DEFAULT_MODE,
    .ai_probability_threshold = CONFIG_ACR_TRIGGER_DEFAULT_AI_PROBABILITY_THRESHOLD_X100 / 100.0,
};
static int64_t s_last_cycle_ms;

static int64_t monotonic_ms(void)
{
    return esp_log_timestamp();
}

static uint32_t normalize_interval_ms(uint32_t interval_ms)
{
    if (interval_ms < 1000) {
        return 1000;
    }
    if (interval_ms > 3600000) {
        return 3600000;
    }
    return interval_ms;
}

static uint32_t normalize_capture_duration_seconds(uint32_t duration_seconds)
{
    if (duration_seconds < AUDIO_CAPTURE_MIN_SECONDS) {
        return AUDIO_CAPTURE_MIN_SECONDS;
    }
    if (duration_seconds > AUDIO_CAPTURE_MAX_SECONDS) {
        return AUDIO_CAPTURE_MAX_SECONDS;
    }
    return duration_seconds;
}

static double normalize_digital_gain(double gain)
{
    if (gain < 0.25) {
        return 0.25;
    }
    if (gain > 16.0) {
        return 16.0;
    }
    return gain;
}

static uint32_t normalize_silence_threshold_rms(uint32_t threshold)
{
    if (threshold > 32767) {
        return 32767;
    }
    return threshold;
}

static uint32_t normalize_silence_hysteresis_rms(uint32_t hysteresis)
{
    if (hysteresis > 32767) {
        return 32767;
    }
    return hysteresis;
}

static uint32_t normalize_min_active_ms(uint32_t min_active_ms)
{
    if (min_active_ms > AUDIO_CAPTURE_MAX_SECONDS * 1000U) {
        return AUDIO_CAPTURE_MAX_SECONDS * 1000U;
    }
    return min_active_ms;
}

static acr_trigger_mode_t normalize_trigger_mode(uint8_t mode)
{
    switch ((acr_trigger_mode_t)mode) {
    case ACR_TRIGGER_MODE_PREDICTION_ONLY:
    case ACR_TRIGGER_MODE_PROBABILITY_ONLY:
    case ACR_TRIGGER_MODE_PREDICTION_OR_PROBABILITY:
    case ACR_TRIGGER_MODE_PREDICTION_AND_PROBABILITY:
        return (acr_trigger_mode_t)mode;
    default:
        return CONFIG_ACR_TRIGGER_DEFAULT_MODE;
    }
}

static double normalize_threshold(double threshold)
{
    if (threshold < 0.0) {
        return 0.0;
    }
    if (threshold > 100.0) {
        return 100.0;
    }
    return threshold;
}

static void load_config(void)
{
    nvs_handle_t handle = 0;
    uint8_t auto_enabled = CONFIG_ACR_AUTO_ENABLED ? 1 : 0;
    uint8_t trigger_mode = CONFIG_ACR_TRIGGER_DEFAULT_MODE;
    uint32_t interval_ms = CONFIG_ACR_AUTO_INTERVAL_MS;
    uint32_t capture_duration_seconds = CONFIG_ACR_CAPTURE_DURATION_SECONDS;
    uint32_t digital_gain_x100 = CONFIG_ACR_DIGITAL_GAIN_X100;
    uint32_t silence_threshold_rms = CONFIG_ACR_SILENCE_THRESHOLD_RMS;
    uint32_t silence_hysteresis_rms = CONFIG_ACR_SILENCE_HYSTERESIS_RMS;
    uint32_t min_active_ms = CONFIG_ACR_MIN_ACTIVE_MS;
    uint32_t threshold_x100 = CONFIG_ACR_TRIGGER_DEFAULT_AI_PROBABILITY_THRESHOLD_X100;
    esp_err_t err = nvs_open(ACR_ANALYSIS_NVS_NAMESPACE, NVS_READONLY, &handle);

    if (err == ESP_OK) {
        (void)nvs_get_u8(handle, ACR_ANALYSIS_NVS_AUTO_KEY, &auto_enabled);
        (void)nvs_get_u32(handle, ACR_ANALYSIS_NVS_INTERVAL_KEY, &interval_ms);
        (void)nvs_get_u32(handle, ACR_ANALYSIS_NVS_CAPTURE_DURATION_KEY, &capture_duration_seconds);
        (void)nvs_get_u32(handle, ACR_ANALYSIS_NVS_DIGITAL_GAIN_KEY, &digital_gain_x100);
        (void)nvs_get_u32(handle, ACR_ANALYSIS_NVS_SILENCE_THRESHOLD_KEY, &silence_threshold_rms);
        (void)nvs_get_u32(handle, ACR_ANALYSIS_NVS_SILENCE_HYSTERESIS_KEY, &silence_hysteresis_rms);
        (void)nvs_get_u32(handle, ACR_ANALYSIS_NVS_MIN_ACTIVE_MS_KEY, &min_active_ms);
        (void)nvs_get_u8(handle, ACR_ANALYSIS_NVS_TRIGGER_MODE_KEY, &trigger_mode);
        (void)nvs_get_u32(handle, ACR_ANALYSIS_NVS_THRESHOLD_X100_KEY, &threshold_x100);
        nvs_close(handle);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Falha ao abrir NVS de controle ACR: %s", esp_err_to_name(err));
    }

    s_config.automatic_enabled = auto_enabled != 0;
    s_config.automatic_interval_ms = normalize_interval_ms(interval_ms);
    s_config.capture_duration_seconds = normalize_capture_duration_seconds(capture_duration_seconds);
    s_config.digital_gain = normalize_digital_gain(digital_gain_x100 / 100.0);
    s_config.silence_threshold_rms = normalize_silence_threshold_rms(silence_threshold_rms);
    s_config.silence_hysteresis_rms = normalize_silence_hysteresis_rms(silence_hysteresis_rms);
    s_config.min_active_ms = normalize_min_active_ms(min_active_ms);
    s_config.trigger_mode = normalize_trigger_mode(trigger_mode);
    s_config.ai_probability_threshold = normalize_threshold(threshold_x100 / 100.0);
    ESP_LOGI(TAG,
             "Controle ACR: automatico=%s intervalo=%ums captura=%us ganho=%.2fx silencio_rms=%u hysteresis_rms=%u min_active=%ums trigger_mode=%d threshold=%.2f%%",
             s_config.automatic_enabled ? "on" : "off",
             (unsigned)s_config.automatic_interval_ms,
             (unsigned)s_config.capture_duration_seconds,
             s_config.digital_gain,
             (unsigned)s_config.silence_threshold_rms,
             (unsigned)s_config.silence_hysteresis_rms,
             (unsigned)s_config.min_active_ms,
             (int)s_config.trigger_mode,
             s_config.ai_probability_threshold);
}

void acr_analysis_control_init(void)
{
    if (!s_event_group) {
        s_event_group = xEventGroupCreate();
    }
    load_config();
    s_last_cycle_ms = monotonic_ms();
}

void acr_analysis_control_request(void)
{
    if (s_event_group) {
        xEventGroupSetBits(s_event_group, ACR_ANALYSIS_REQUEST_BIT);
    }
}

acr_analysis_trigger_t acr_analysis_control_wait(TickType_t timeout_ticks)
{
    TickType_t wait_ticks = timeout_ticks;

    if (!s_event_group) {
        return ACR_ANALYSIS_TRIGGER_NONE;
    }

    if (s_config.automatic_enabled) {
        int64_t now_ms = monotonic_ms();
        int64_t elapsed_ms = now_ms - s_last_cycle_ms;
        int64_t remaining_ms = (int64_t)s_config.automatic_interval_ms - elapsed_ms;

        if (remaining_ms <= 0) {
            return ACR_ANALYSIS_TRIGGER_AUTO;
        }
        if (timeout_ticks == portMAX_DELAY ||
            pdMS_TO_TICKS(remaining_ms) < timeout_ticks) {
            wait_ticks = pdMS_TO_TICKS(remaining_ms);
            if (wait_ticks == 0) {
                wait_ticks = 1;
            }
        }
    }

    EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                           ACR_ANALYSIS_REQUEST_BIT | ACR_ANALYSIS_CONFIG_CHANGED_BIT,
                                           pdTRUE,
                                           pdFALSE,
                                           wait_ticks);
    if (bits & ACR_ANALYSIS_REQUEST_BIT) {
        return ACR_ANALYSIS_TRIGGER_MANUAL;
    }
    if (bits & ACR_ANALYSIS_CONFIG_CHANGED_BIT) {
        return ACR_ANALYSIS_TRIGGER_NONE;
    }
    if (s_config.automatic_enabled) {
        return ACR_ANALYSIS_TRIGGER_AUTO;
    }
    return ACR_ANALYSIS_TRIGGER_NONE;
}

void acr_analysis_control_set_last_cycle_ms(int64_t timestamp_ms)
{
    s_last_cycle_ms = timestamp_ms > 0 ? timestamp_ms : monotonic_ms();
}

esp_err_t acr_analysis_control_get_config(acr_analysis_control_config_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = s_config;
    return ESP_OK;
}

esp_err_t acr_analysis_control_save_config(const acr_analysis_control_config_t *config)
{
    nvs_handle_t handle = 0;
    esp_err_t err = ESP_OK;

    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    s_config.automatic_enabled = config->automatic_enabled;
    s_config.automatic_interval_ms = normalize_interval_ms(config->automatic_interval_ms);
    s_config.capture_duration_seconds =
        normalize_capture_duration_seconds(config->capture_duration_seconds);
    s_config.digital_gain = normalize_digital_gain(config->digital_gain);
    s_config.silence_threshold_rms =
        normalize_silence_threshold_rms(config->silence_threshold_rms);
    s_config.silence_hysteresis_rms =
        normalize_silence_hysteresis_rms(config->silence_hysteresis_rms);
    s_config.min_active_ms = normalize_min_active_ms(config->min_active_ms);
    s_config.trigger_mode = normalize_trigger_mode((uint8_t)config->trigger_mode);
    s_config.ai_probability_threshold = normalize_threshold(config->ai_probability_threshold);

    err = nvs_open(ACR_ANALYSIS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, ACR_ANALYSIS_NVS_AUTO_KEY, s_config.automatic_enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, ACR_ANALYSIS_NVS_INTERVAL_KEY, s_config.automatic_interval_ms);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle,
                          ACR_ANALYSIS_NVS_CAPTURE_DURATION_KEY,
                          s_config.capture_duration_seconds);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle,
                          ACR_ANALYSIS_NVS_DIGITAL_GAIN_KEY,
                          (uint32_t)(s_config.digital_gain * 100.0 + 0.5));
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle,
                          ACR_ANALYSIS_NVS_SILENCE_THRESHOLD_KEY,
                          s_config.silence_threshold_rms);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle,
                          ACR_ANALYSIS_NVS_SILENCE_HYSTERESIS_KEY,
                          s_config.silence_hysteresis_rms);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle,
                          ACR_ANALYSIS_NVS_MIN_ACTIVE_MS_KEY,
                          s_config.min_active_ms);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, ACR_ANALYSIS_NVS_TRIGGER_MODE_KEY, (uint8_t)s_config.trigger_mode);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle,
                          ACR_ANALYSIS_NVS_THRESHOLD_X100_KEY,
                          (uint32_t)(s_config.ai_probability_threshold * 100.0 + 0.5));
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK && s_event_group) {
        xEventGroupSetBits(s_event_group, ACR_ANALYSIS_CONFIG_CHANGED_BIT);
    }
    return err;
}
