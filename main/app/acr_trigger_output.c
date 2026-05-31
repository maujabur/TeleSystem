#include "acr_trigger_output.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "status_led.h"

#ifndef CONFIG_ACR_TRIGGER_GPIO
#define CONFIG_ACR_TRIGGER_GPIO 5
#endif

#ifndef CONFIG_ACR_TRIGGER_ACTIVE_LEVEL
#define CONFIG_ACR_TRIGGER_ACTIVE_LEVEL 1
#endif

#ifndef CONFIG_ACR_TRIGGER_PULSE_MS
#define CONFIG_ACR_TRIGGER_PULSE_MS 2200
#endif

#define ACR_TRIGGER_NVS_NAMESPACE "acr_trigger"
#define ACR_TRIGGER_NVS_ENABLED_KEY "enabled"
#define ACR_TRIGGER_NVS_GPIO_KEY "gpio"
#define ACR_TRIGGER_NVS_ACTIVE_LEVEL_KEY "active"
#define ACR_TRIGGER_NVS_PULSE_MS_KEY "pulse_ms"

static const char *TAG = "acr-trigger";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static int s_configured_gpio = -1;
static acr_trigger_output_status_t s_status = {
    .config = {
        .enabled = CONFIG_ACR_TRIGGER_GPIO >= 0,
        .gpio = CONFIG_ACR_TRIGGER_GPIO,
        .active_level = CONFIG_ACR_TRIGGER_ACTIVE_LEVEL,
        .pulse_ms = CONFIG_ACR_TRIGGER_PULSE_MS,
    },
    .last_pulse_at_ms = 0,
    .last_error = ESP_OK,
};

static int inactive_level_for(const acr_trigger_output_config_t *config)
{
    return config->active_level ? 0 : 1;
}

static esp_err_t normalize_config(const acr_trigger_output_config_t *in,
                                  acr_trigger_output_config_t *out)
{
    if (!in || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = *in;
    out->active_level = out->active_level ? 1 : 0;
    if (out->pulse_ms < 10 || out->pulse_ms > 60000) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out->enabled && (out->gpio < 0 || out->gpio > 48)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!out->enabled) {
        out->gpio = in->gpio;
    }
    return ESP_OK;
}

static esp_err_t configure_gpio(const acr_trigger_output_config_t *config)
{
    if (!config->enabled || config->gpio < 0) {
        ESP_LOGW(TAG, "GPIO de trigger ACR desabilitada");
        s_configured_gpio = -1;
        return ESP_OK;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << config->gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);

    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Falha ao configurar GPIO de trigger ACR %d: %s",
                 config->gpio,
                 esp_err_to_name(err));
        return err;
    }

    gpio_set_level((gpio_num_t)config->gpio, inactive_level_for(config));
    s_configured_gpio = config->gpio;
    ESP_LOGI(TAG,
             "GPIO de trigger ACR configurada: enabled=%s gpio=%d active=%d pulse=%ums",
             config->enabled ? "sim" : "nao",
             config->gpio,
             config->active_level,
             (unsigned)config->pulse_ms);
    return ESP_OK;
}

static void load_config_from_nvs(acr_trigger_output_config_t *config)
{
    nvs_handle_t handle = 0;
    uint8_t enabled = config->enabled ? 1 : 0;
    uint8_t active_level = config->active_level ? 1 : 0;
    int32_t gpio = config->gpio;
    uint32_t pulse_ms = config->pulse_ms;
    esp_err_t err = nvs_open(ACR_TRIGGER_NVS_NAMESPACE, NVS_READONLY, &handle);

    if (err == ESP_OK) {
        (void)nvs_get_u8(handle, ACR_TRIGGER_NVS_ENABLED_KEY, &enabled);
        (void)nvs_get_i32(handle, ACR_TRIGGER_NVS_GPIO_KEY, &gpio);
        (void)nvs_get_u8(handle, ACR_TRIGGER_NVS_ACTIVE_LEVEL_KEY, &active_level);
        (void)nvs_get_u32(handle, ACR_TRIGGER_NVS_PULSE_MS_KEY, &pulse_ms);
        nvs_close(handle);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Falha ao abrir NVS de trigger ACR: %s", esp_err_to_name(err));
    }

    config->enabled = enabled != 0;
    config->gpio = gpio;
    config->active_level = active_level ? 1 : 0;
    config->pulse_ms = pulse_ms;
    if (normalize_config(config, config) != ESP_OK) {
        ESP_LOGW(TAG, "Configuracao BT_NEXT invalida na NVS, usando defaults");
        config->enabled = CONFIG_ACR_TRIGGER_GPIO >= 0;
        config->gpio = CONFIG_ACR_TRIGGER_GPIO;
        config->active_level = CONFIG_ACR_TRIGGER_ACTIVE_LEVEL ? 1 : 0;
        config->pulse_ms = CONFIG_ACR_TRIGGER_PULSE_MS;
    }
}

static esp_err_t save_config_to_nvs(const acr_trigger_output_config_t *config)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(ACR_TRIGGER_NVS_NAMESPACE, NVS_READWRITE, &handle);

    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, ACR_TRIGGER_NVS_ENABLED_KEY, config->enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, ACR_TRIGGER_NVS_GPIO_KEY, config->gpio);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, ACR_TRIGGER_NVS_ACTIVE_LEVEL_KEY, config->active_level ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, ACR_TRIGGER_NVS_PULSE_MS_KEY, config->pulse_ms);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t acr_trigger_output_init(void)
{
    acr_trigger_output_config_t config = s_status.config;

    load_config_from_nvs(&config);
    esp_err_t err = configure_gpio(&config);

    portENTER_CRITICAL(&s_lock);
    s_status.config = config;
    s_status.last_error = err;
    s_initialized = err == ESP_OK;
    portEXIT_CRITICAL(&s_lock);
    return err;
}

esp_err_t acr_trigger_output_get_status(acr_trigger_output_status_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t acr_trigger_output_save_config(const acr_trigger_output_config_t *config)
{
    acr_trigger_output_config_t normalized = {0};
    acr_trigger_output_config_t old_config = {0};
    esp_err_t err = normalize_config(config, &normalized);

    if (err != ESP_OK) {
        return err;
    }

    portENTER_CRITICAL(&s_lock);
    old_config = s_status.config;
    portEXIT_CRITICAL(&s_lock);

    if (s_configured_gpio >= 0 &&
        (!normalized.enabled || normalized.gpio != old_config.gpio ||
         normalized.active_level != old_config.active_level)) {
        gpio_set_level((gpio_num_t)s_configured_gpio, inactive_level_for(&old_config));
    }

    err = configure_gpio(&normalized);
    if (err == ESP_OK) {
        err = save_config_to_nvs(&normalized);
    }

    portENTER_CRITICAL(&s_lock);
    if (err == ESP_OK) {
        s_status.config = normalized;
        s_initialized = true;
    }
    s_status.last_error = err;
    portEXIT_CRITICAL(&s_lock);

    return err;
}

esp_err_t acr_trigger_output_pulse(void)
{
    acr_trigger_output_config_t config = {0};
    bool initialized = false;

    portENTER_CRITICAL(&s_lock);
    config = s_status.config;
    initialized = s_initialized;
    portEXIT_CRITICAL(&s_lock);

    if (!initialized || !config.enabled || config.gpio < 0) {
        portENTER_CRITICAL(&s_lock);
        s_status.last_error = ESP_ERR_INVALID_STATE;
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Acionando GPIO de trigger ACR por %ums", (unsigned)config.pulse_ms);
    status_led_state_t previous_led_state = status_led_get_state();
    status_led_set_state(STATUS_LED_STATE_BT_NEXT_ACTIVE);
    gpio_set_level((gpio_num_t)config.gpio, config.active_level ? 1 : 0);
    vTaskDelay(pdMS_TO_TICKS(config.pulse_ms));
    gpio_set_level((gpio_num_t)config.gpio, inactive_level_for(&config));
    status_led_set_state(previous_led_state);

    portENTER_CRITICAL(&s_lock);
    s_status.last_pulse_at_ms = esp_log_timestamp();
    s_status.last_error = ESP_OK;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}
