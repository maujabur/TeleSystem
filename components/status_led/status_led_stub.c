#include "status_led.h"

static status_led_state_t s_state = STATUS_LED_STATE_BOOT;
static status_led_signal_t s_signal = {
    .pattern = STATUS_LED_PATTERN_BREATH,
    .color = {
        .red = 0x20,
        .green = 0x20,
        .blue = 0x20,
    },
};

esp_err_t status_led_start(void)
{
    s_state = STATUS_LED_STATE_BOOT;
    return ESP_OK;
}

status_led_state_t status_led_get_state(void)
{
    return s_state;
}

esp_err_t status_led_set_state(status_led_state_t state)
{
    if (state > STATUS_LED_STATE_LOW_BATTERY) {
        return ESP_ERR_INVALID_ARG;
    }

    s_state = state;
    return ESP_OK;
}

esp_err_t status_led_set_signal(const status_led_signal_t *signal)
{
    if (!signal || signal->pattern > STATUS_LED_PATTERN_BLINK_FAST) {
        return ESP_ERR_INVALID_ARG;
    }

    s_signal = *signal;
    return ESP_OK;
}

esp_err_t status_led_get_signal(status_led_signal_t *out_signal)
{
    if (!out_signal) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_signal = s_signal;
    return ESP_OK;
}

esp_err_t status_led_set_capture_overlay(bool enabled)
{
    (void)enabled;
    return ESP_OK;
}
