#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STATUS_LED_STATE_BOOT = 0,
    STATUS_LED_STATE_WIFI_CONNECTING,
    STATUS_LED_STATE_WIFI_PROVISIONING,
    STATUS_LED_STATE_WIFI_CONNECTED,
    STATUS_LED_STATE_PRODUCT_TRANSMITTING,
    STATUS_LED_STATE_PRODUCT_WAITING,
    STATUS_LED_STATE_PRODUCT_RESULT_OK,
    STATUS_LED_STATE_PRODUCT_RESULT_ALERT,
    STATUS_LED_STATE_OUTPUT_ACTIVE,
    STATUS_LED_STATE_ERROR,
    STATUS_LED_STATE_LOW_BATTERY,
} status_led_state_t;

typedef enum {
    STATUS_LED_PATTERN_OFF = 0,
    STATUS_LED_PATTERN_SOLID,
    STATUS_LED_PATTERN_BREATH,
    STATUS_LED_PATTERN_BLINK_SLOW,
    STATUS_LED_PATTERN_BLINK_FAST,
} status_led_pattern_t;

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} status_led_color_t;

typedef struct {
    status_led_pattern_t pattern;
    status_led_color_t color;
} status_led_signal_t;

esp_err_t status_led_start(void);
status_led_state_t status_led_get_state(void);
esp_err_t status_led_set_state(status_led_state_t state);
esp_err_t status_led_set_signal(const status_led_signal_t *signal);
esp_err_t status_led_get_signal(status_led_signal_t *out_signal);
esp_err_t status_led_set_capture_overlay(bool enabled);

#ifdef __cplusplus
}
#endif

#endif
