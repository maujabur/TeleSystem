#include "status_led.h"

static status_led_state_t s_state = STATUS_LED_STATE_BOOT;

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

esp_err_t status_led_set_capture_overlay(bool enabled)
{
    (void)enabled;
    return ESP_OK;
}
