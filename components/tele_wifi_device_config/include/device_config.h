#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include "esp_err.h"

#define DEVICE_CONFIG_PROVISIONING_SSID_BUFFER_SIZE 33
#define DEVICE_CONFIG_STA_MAX_RETRY_MIN 1
#define DEVICE_CONFIG_STA_MAX_RETRY_MAX 20
#define DEVICE_CONFIG_APSTA_GRACE_PERIOD_S_MIN 30
#define DEVICE_CONFIG_APSTA_GRACE_PERIOD_S_MAX 3600
#define DEVICE_CONFIG_ID_PROVISIONING_SSID "wifi.provisioning_ssid"
#define DEVICE_CONFIG_ID_STA_MAX_RETRY "wifi.sta_max_retry"
#define DEVICE_CONFIG_ID_APSTA_POLICY "wifi.apsta_policy"
#define DEVICE_CONFIG_ID_APSTA_GRACE_PERIOD_S "wifi.apsta_grace_period_s"

typedef enum {
    DEVICE_CONFIG_APSTA_ALWAYS_ON = 0,
    DEVICE_CONFIG_APSTA_AUTO_TIMEOUT,
    DEVICE_CONFIG_APSTA_STA_ONLY,
} device_config_apsta_policy_t;

esp_err_t device_config_register_fields(void);

#endif
