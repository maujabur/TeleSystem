#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdbool.h>
#include <stdint.h>

#ifdef STATUS_LED_HOST_TEST
#include "status_led_host_stubs.h"
#else
#include "esp_err.h"
#endif

#include "tele_signal.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t status_led_start(void);
esp_err_t status_led_apply_effect(const tele_signal_effect_t *effect);
esp_err_t status_led_off(void);
const char *const *status_led_supported_effect_ids(void);
uint8_t status_led_supported_effect_count(void);
bool status_led_effect_supported(const char *effect_id);
esp_err_t status_led_get_current_effect(tele_signal_effect_t *out_effect);
esp_err_t status_led_set_capture_overlay(bool enabled);

#ifdef __cplusplus
}
#endif

#endif
