#include "status_led.h"

#include <string.h>

static tele_signal_effect_t s_current_effect = {
    .id = TELE_SIGNAL_EFFECT_OFF,
    .target_mask = TELE_SIGNAL_TARGET_ALL,
};

esp_err_t status_led_start(void)
{
    return ESP_OK;
}

esp_err_t status_led_apply_effect(const tele_signal_effect_t *effect)
{
    if (!effect) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!status_led_effect_supported(effect->id)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_current_effect = *effect;
    return ESP_OK;
}

esp_err_t status_led_off(void)
{
    const tele_signal_effect_t off_effect = {
        .id = TELE_SIGNAL_EFFECT_OFF,
        .target_mask = TELE_SIGNAL_TARGET_ALL,
    };
    return status_led_apply_effect(&off_effect);
}

esp_err_t status_led_get_current_effect(tele_signal_effect_t *out_effect)
{
    if (!out_effect) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_effect = s_current_effect;
    return ESP_OK;
}

esp_err_t status_led_set_capture_overlay(bool enabled)
{
    (void)enabled;
    return ESP_OK;
}
