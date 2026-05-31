#include "vbat_monitor.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/portmacro.h"

static const char *TAG = "vbat-monitor";

#ifndef CONFIG_VBAT_MONITOR_ENABLED
#define CONFIG_VBAT_MONITOR_ENABLED 0
#endif

#ifndef CONFIG_VBAT_MONITOR_GPIO
#define CONFIG_VBAT_MONITOR_GPIO 1
#endif

#ifndef CONFIG_VBAT_MONITOR_MAX_INTERVAL_S
#define CONFIG_VBAT_MONITOR_MAX_INTERVAL_S 120
#endif

#ifndef CONFIG_VBAT_MONITOR_ON_BOOT
#define CONFIG_VBAT_MONITOR_ON_BOOT 1
#endif

#ifndef CONFIG_VBAT_MONITOR_ON_BEFORE_WIFI_PS_EXIT
#define CONFIG_VBAT_MONITOR_ON_BEFORE_WIFI_PS_EXIT 1
#endif

#ifndef CONFIG_VBAT_MONITOR_ON_AFTER_ACR_TX
#define CONFIG_VBAT_MONITOR_ON_AFTER_ACR_TX 1
#endif

#ifndef CONFIG_VBAT_MONITOR_ON_AFTER_AUDIO_CAPTURE
#define CONFIG_VBAT_MONITOR_ON_AFTER_AUDIO_CAPTURE 1
#endif

#ifndef CONFIG_VBAT_MONITOR_ON_MAX_INTERVAL
#define CONFIG_VBAT_MONITOR_ON_MAX_INTERVAL 1
#endif

#ifndef CONFIG_VBAT_SHUTDOWN_ENABLED
#define CONFIG_VBAT_SHUTDOWN_ENABLED 0
#endif

#ifndef CONFIG_VBAT_SHUTDOWN_THRESHOLD_MV
#define CONFIG_VBAT_SHUTDOWN_THRESHOLD_MV 3500
#endif

#ifndef CONFIG_VBAT_SHUTDOWN_DEBOUNCE_MS
#define CONFIG_VBAT_SHUTDOWN_DEBOUNCE_MS 1000
#endif

#ifndef CONFIG_VBAT_MAINTENANCE_MODE_MAX_MV
#define CONFIG_VBAT_MAINTENANCE_MODE_MAX_MV 500
#endif

#ifndef CONFIG_VBAT_MONITOR_RDIV_RVCC
#define CONFIG_VBAT_MONITOR_RDIV_RVCC 4640
#endif

#ifndef CONFIG_VBAT_MONITOR_RDIV_RGND
#define CONFIG_VBAT_MONITOR_RDIV_RGND 5620
#endif

#define VBAT_MONITOR_SAMPLE_COUNT 32

typedef struct {
    bool enabled;
    bool initialized;
    bool calibrated;
    int gpio;
    adc_unit_t unit;
    adc_channel_t channel;
    adc_oneshot_unit_handle_t adc_handle;
    adc_cali_handle_t cali_handle;
    int64_t last_measurement_ms;
    int64_t below_threshold_since_ms;
} vbat_monitor_state_t;

static vbat_monitor_state_t s_state = {
    .enabled = CONFIG_VBAT_MONITOR_ENABLED,
    .initialized = false,
    .calibrated = false,
    .gpio = CONFIG_VBAT_MONITOR_GPIO,
    .unit = ADC_UNIT_1,
    .channel = ADC_CHANNEL_0,
    .adc_handle = NULL,
    .cali_handle = NULL,
    .last_measurement_ms = 0,
    .below_threshold_since_ms = 0,
};

static vbat_monitor_status_t s_status = {
    .enabled = CONFIG_VBAT_MONITOR_ENABLED,
    .initialized = false,
    .calibrated = false,
    .shutdown_enabled = CONFIG_VBAT_SHUTDOWN_ENABLED,
    .shutdown_countdown_active = false,
    .gpio = CONFIG_VBAT_MONITOR_GPIO,
    .raw_avg = 0,
    .gpio_mv = 0,
    .vbat_mv = 0,
    .shutdown_threshold_mv = CONFIG_VBAT_SHUTDOWN_THRESHOLD_MV,
    .shutdown_debounce_ms = CONFIG_VBAT_SHUTDOWN_DEBOUNCE_MS,
    .last_measurement_ms = 0,
    .shutdown_below_threshold_since_ms = 0,
    .last_moment = VBAT_MONITOR_MOMENT_BOOT,
    .measurement_count = 0,
    .maintenance_mode = false,
};

static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;

static void history_csv_reset(void)
{
    ESP_LOGI(TAG, "Historico CSV de VBAT desabilitado (sem escrita em /data)");
}

static void history_csv_append(vbat_monitor_moment_t moment, int vbat_mv, int64_t timestamp_ms)
{
    (void)moment;
    (void)vbat_mv;
    (void)timestamp_ms;
}

const char *vbat_monitor_moment_name(vbat_monitor_moment_t moment)
{
    switch (moment) {
    case VBAT_MONITOR_MOMENT_BOOT:
        return "boot";
    case VBAT_MONITOR_MOMENT_BEFORE_WIFI_PS_EXIT:
        return "before_wifi_ps_exit";
    case VBAT_MONITOR_MOMENT_AFTER_ACR_TX:
        return "after_acr_tx";
    case VBAT_MONITOR_MOMENT_AFTER_AUDIO_CAPTURE:
        return "after_audio_capture";
    case VBAT_MONITOR_MOMENT_MAX_INTERVAL:
        return "max_interval";
    default:
        return "unknown";
    }
}

static void status_set_initialized(bool initialized)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.initialized = initialized;
    portEXIT_CRITICAL(&s_status_lock);
}

static void status_set_calibrated(bool calibrated)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.calibrated = calibrated;
    portEXIT_CRITICAL(&s_status_lock);
}

static void status_note_failed_measurement(int64_t timestamp_ms)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.last_measurement_ms = timestamp_ms;
    portEXIT_CRITICAL(&s_status_lock);
}

static void status_set_measurement(vbat_monitor_moment_t moment,
                                   int raw_avg,
                                   int gpio_mv,
                                   int vbat_mv,
                                   int64_t timestamp_ms)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.raw_avg = raw_avg;
    s_status.gpio_mv = gpio_mv;
    s_status.vbat_mv = vbat_mv;
    s_status.last_measurement_ms = timestamp_ms;
    s_status.last_moment = moment;
    s_status.measurement_count++;
    portEXIT_CRITICAL(&s_status_lock);

    if (CONFIG_VBAT_SHUTDOWN_ENABLED) {
        bool in_maintenance = (vbat_mv <= CONFIG_VBAT_MAINTENANCE_MODE_MAX_MV);

        if (in_maintenance) {
            if (s_state.below_threshold_since_ms != 0) {
                ESP_LOGI(TAG,
                         "VBAT em modo manutencao | vbat_mv=%d <= maintenance_max=%d | debounce cancelado",
                         vbat_mv,
                         CONFIG_VBAT_MAINTENANCE_MODE_MAX_MV);
            }
            s_state.below_threshold_since_ms = 0;
        } else if (vbat_mv < CONFIG_VBAT_SHUTDOWN_THRESHOLD_MV) {
            if (s_state.below_threshold_since_ms == 0) {
                s_state.below_threshold_since_ms = timestamp_ms;
                ESP_LOGW(TAG,
                         "VBAT abaixo do limiar | vbat_mv=%d < threshold=%d | debounce iniciado",
                         vbat_mv,
                         CONFIG_VBAT_SHUTDOWN_THRESHOLD_MV);
            }
        } else {
            if (s_state.below_threshold_since_ms != 0) {
                ESP_LOGI(TAG,
                         "VBAT recuperada | vbat_mv=%d >= threshold=%d | debounce cancelado",
                         vbat_mv,
                         CONFIG_VBAT_SHUTDOWN_THRESHOLD_MV);
            }
            s_state.below_threshold_since_ms = 0;
        }

        portENTER_CRITICAL(&s_status_lock);
        s_status.maintenance_mode = in_maintenance;
        s_status.shutdown_countdown_active = s_state.below_threshold_since_ms > 0;
        s_status.shutdown_below_threshold_since_ms = s_state.below_threshold_since_ms;
        portEXIT_CRITICAL(&s_status_lock);
    }
}

static bool moment_enabled(vbat_monitor_moment_t moment)
{
    switch (moment) {
    case VBAT_MONITOR_MOMENT_BOOT:
        return CONFIG_VBAT_MONITOR_ON_BOOT;
    case VBAT_MONITOR_MOMENT_BEFORE_WIFI_PS_EXIT:
        return CONFIG_VBAT_MONITOR_ON_BEFORE_WIFI_PS_EXIT;
    case VBAT_MONITOR_MOMENT_AFTER_ACR_TX:
        return CONFIG_VBAT_MONITOR_ON_AFTER_ACR_TX;
    case VBAT_MONITOR_MOMENT_AFTER_AUDIO_CAPTURE:
        return CONFIG_VBAT_MONITOR_ON_AFTER_AUDIO_CAPTURE;
    case VBAT_MONITOR_MOMENT_MAX_INTERVAL:
        return CONFIG_VBAT_MONITOR_ON_MAX_INTERVAL;
    default:
        return false;
    }
}

static bool max_interval_enabled(void)
{
    return s_state.enabled &&
           CONFIG_VBAT_MONITOR_ON_MAX_INTERVAL &&
           CONFIG_VBAT_MONITOR_MAX_INTERVAL_S > 0;
}

static esp_err_t create_cali_handle(adc_unit_t unit,
                                    adc_channel_t channel,
                                    adc_atten_t atten,
                                    adc_cali_handle_t *out_handle)
{
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = unit,
        .chan = channel,
        .atten = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_curve_fitting(&cali_cfg, out_handle);
    if (err == ESP_OK) {
        return ESP_OK;
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = unit,
        .atten = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_line_fitting(&cali_cfg, out_handle);
    if (err == ESP_OK) {
        return ESP_OK;
    }
#endif

    return err;
}

static bool interval_due(void)
{
    int64_t now_ms = 0;
    int64_t elapsed_ms = 0;

    if (!max_interval_enabled()) {
        return false;
    }

    now_ms = esp_log_timestamp();
    elapsed_ms = now_ms - s_state.last_measurement_ms;
    return elapsed_ms >= ((int64_t)CONFIG_VBAT_MONITOR_MAX_INTERVAL_S * 1000);
}

esp_err_t vbat_monitor_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {0};
    adc_oneshot_chan_cfg_t chan_cfg;
    esp_err_t err = ESP_OK;
    gpio_num_t gpio_num = (gpio_num_t)s_state.gpio;

    s_state.last_measurement_ms = esp_log_timestamp();
    status_note_failed_measurement(s_state.last_measurement_ms);

    if (!s_state.enabled) {
        ESP_LOGI(TAG, "Monitor de VBAT desabilitado por configuracao");
        return ESP_OK;
    }

    history_csv_reset();

    err = adc_oneshot_io_to_channel(gpio_num, &s_state.unit, &s_state.channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "GPIO %d nao suporta ADC oneshot: %s",
                 s_state.gpio,
                 esp_err_to_name(err));
        return err;
    }

    unit_cfg.unit_id = s_state.unit;
    unit_cfg.ulp_mode = ADC_ULP_MODE_DISABLE;
    err = adc_oneshot_new_unit(&unit_cfg, &s_state.adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao criar unidade ADC: %s", esp_err_to_name(err));
        return err;
    }

    chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    chan_cfg.atten = ADC_ATTEN_DB_12;
    err = adc_oneshot_config_channel(s_state.adc_handle, s_state.channel, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar canal ADC: %s", esp_err_to_name(err));
        return err;
    }

    err = create_cali_handle(s_state.unit, s_state.channel, chan_cfg.atten, &s_state.cali_handle);
    if (err == ESP_OK) {
        s_state.calibrated = true;
    } else {
        s_state.calibrated = false;
    }
    status_set_calibrated(s_state.calibrated);
    ESP_LOGI(TAG,
             "VBAT ADC calibration: %s",
             s_state.calibrated ? "enabled" : "disabled");

    s_state.initialized = true;
    status_set_initialized(true);
    ESP_LOGI(TAG,
             "VBAT monitor pronto | gpio=%d | unit=%d | channel=%d",
             s_state.gpio,
             (int)s_state.unit,
             (int)s_state.channel);
    return ESP_OK;
}

void vbat_monitor_maybe_measure(vbat_monitor_moment_t moment)
{
    int raw = 0;
    int raw_sum = 0;
    int raw_avg = 0;
    int gpio_mv = 0;
    int vbat_mv = 0;
    esp_err_t err = ESP_OK;

    if (!s_state.enabled || !moment_enabled(moment)) {
        return;
    }

    if (moment == VBAT_MONITOR_MOMENT_MAX_INTERVAL && !interval_due()) {
        return;
    }

    if (!s_state.initialized) {
        ESP_LOGW(TAG,
                 "Medicao ignorada (%s): monitor nao inicializado",
                 vbat_monitor_moment_name(moment));
        return;
    }

    for (int i = 0; i < VBAT_MONITOR_SAMPLE_COUNT; ++i) {
        err = adc_oneshot_read(s_state.adc_handle, s_state.channel, &raw);
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "Falha em leitura ADC (%s): %s",
                     vbat_monitor_moment_name(moment),
                     esp_err_to_name(err));
            s_state.last_measurement_ms = esp_log_timestamp();
            status_note_failed_measurement(s_state.last_measurement_ms);
            return;
        }
        raw_sum += raw;
    }

    raw_avg = raw_sum / VBAT_MONITOR_SAMPLE_COUNT;
    if (s_state.calibrated) {
        err = adc_cali_raw_to_voltage(s_state.cali_handle, raw_avg, &gpio_mv);
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "Falha em conversao calibrada ADC (%s): %s",
                     vbat_monitor_moment_name(moment),
                     esp_err_to_name(err));
            s_state.last_measurement_ms = esp_log_timestamp();
            status_note_failed_measurement(s_state.last_measurement_ms);
            return;
        }
    } else {
        gpio_mv = (raw_avg * 3300) / 4095;
    }

    vbat_mv = gpio_mv * (CONFIG_VBAT_MONITOR_RDIV_RVCC + CONFIG_VBAT_MONITOR_RDIV_RGND) / CONFIG_VBAT_MONITOR_RDIV_RGND;
    s_state.last_measurement_ms = esp_log_timestamp();
    status_set_measurement(moment, raw_avg, gpio_mv, vbat_mv, s_state.last_measurement_ms);
    history_csv_append(moment, vbat_mv, s_state.last_measurement_ms);

    ESP_LOGI(TAG,
             "VBAT sample | moment=%s | gpio=%d | vbat_mv=%d",
             vbat_monitor_moment_name(moment),
             s_state.gpio,
             vbat_mv);
}

TickType_t vbat_monitor_next_wait_timeout(TickType_t base_timeout)
{
    TickType_t max_interval_ticks = 0;
    int64_t now_ms = 0;
    int64_t remaining_ms = 0;

    if (!max_interval_enabled()) {
        return base_timeout;
    }

    now_ms = esp_log_timestamp();
    remaining_ms = ((int64_t)CONFIG_VBAT_MONITOR_MAX_INTERVAL_S * 1000) -
                   (now_ms - s_state.last_measurement_ms);
    if (remaining_ms <= 0) {
        max_interval_ticks = 1;
    } else {
        max_interval_ticks = pdMS_TO_TICKS(remaining_ms);
        if (max_interval_ticks == 0) {
            max_interval_ticks = 1;
        }
    }

    if (base_timeout == portMAX_DELAY || max_interval_ticks < base_timeout) {
        return max_interval_ticks;
    }

    return base_timeout;
}

esp_err_t vbat_monitor_get_status(vbat_monitor_status_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_status_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_status_lock);
    return ESP_OK;
}

bool vbat_monitor_is_maintenance_mode(void)
{
    if (!CONFIG_VBAT_SHUTDOWN_ENABLED || !s_state.initialized) {
        return false;
    }

    vbat_monitor_status_t status = {0};
    portENTER_CRITICAL(&s_status_lock);
    status = s_status;
    portEXIT_CRITICAL(&s_status_lock);

    return status.measurement_count > 0 && status.maintenance_mode;
}

bool vbat_monitor_is_below_threshold(void)
{
    if (!CONFIG_VBAT_SHUTDOWN_ENABLED || !s_state.initialized) {
        return false;
    }

    vbat_monitor_status_t status = {0};
    portENTER_CRITICAL(&s_status_lock);
    status = s_status;
    portEXIT_CRITICAL(&s_status_lock);

    return status.measurement_count > 0 &&
           !status.maintenance_mode &&
           status.vbat_mv < CONFIG_VBAT_SHUTDOWN_THRESHOLD_MV;
}

bool vbat_monitor_check_shutdown(void)
{
    if (!CONFIG_VBAT_SHUTDOWN_ENABLED || !s_state.initialized) {
        return false;
    }

    /* Modo manutencao: bateria desconectada, inibe desligamento */
    if (vbat_monitor_is_maintenance_mode()) {
        return false;
    }

    if (s_state.below_threshold_since_ms == 0) {
        return false;
    }

    int64_t now_ms = esp_log_timestamp();
    int64_t elapsed_ms = now_ms - s_state.below_threshold_since_ms;
    return elapsed_ms >= (int64_t)CONFIG_VBAT_SHUTDOWN_DEBOUNCE_MS;
}