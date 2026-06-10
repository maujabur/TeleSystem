#ifndef DEVICE_CONFIG_STORE_H
#define DEVICE_CONFIG_STORE_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define DEVICE_CONFIG_PROVISIONING_SSID_BUFFER_SIZE 33
#define DEVICE_CONFIG_STA_MAX_RETRY_MIN 1
#define DEVICE_CONFIG_STA_MAX_RETRY_MAX 20
#define DEVICE_CONFIG_APSTA_GRACE_PERIOD_S_MIN 30
#define DEVICE_CONFIG_APSTA_GRACE_PERIOD_S_MAX 3600

typedef enum {
	DEVICE_CONFIG_APSTA_ALWAYS_ON = 0,
	DEVICE_CONFIG_APSTA_AUTO_TIMEOUT,
	DEVICE_CONFIG_APSTA_STA_ONLY,
} device_config_apsta_policy_t;

esp_err_t device_config_store_load_provisioning_ssid(char *out, size_t out_size);
esp_err_t device_config_store_save_provisioning_ssid(const char *ssid);
esp_err_t device_config_store_load_sta_max_retry(uint8_t *out_retry);
esp_err_t device_config_store_save_sta_max_retry(uint8_t retry);
esp_err_t device_config_store_load_apsta_policy(device_config_apsta_policy_t *out_policy,
												uint32_t *out_grace_period_s);
esp_err_t device_config_store_save_apsta_policy(device_config_apsta_policy_t policy,
												uint32_t grace_period_s);

#endif
