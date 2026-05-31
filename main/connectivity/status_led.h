#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STATUS_LED_STATE_BOOT = 0,
    STATUS_LED_STATE_WIFI_CONNECTING,
    STATUS_LED_STATE_WIFI_PROVISIONING,
    STATUS_LED_STATE_WIFI_CONNECTED,
    STATUS_LED_STATE_ACR_UPLOADING,
    STATUS_LED_STATE_ACR_WAITING_RESULT,
    STATUS_LED_STATE_ACR_RESULT_HUMAN,
    STATUS_LED_STATE_ACR_RESULT_AI,
    STATUS_LED_STATE_BT_NEXT_ACTIVE,
    STATUS_LED_STATE_ERROR,
    STATUS_LED_STATE_LOW_BATTERY,
} status_led_state_t;

esp_err_t status_led_start(void);
status_led_state_t status_led_get_state(void);
esp_err_t status_led_set_state(status_led_state_t state);
esp_err_t status_led_set_capture_overlay(bool enabled);

#ifdef __cplusplus
}
#endif

#endif
