#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define WIFI_CONFIG_SSID_MAX_LEN 32
#define WIFI_CONFIG_PASSWORD_MAX_LEN 64
#define WIFI_CONFIG_MAX_SAVED_NETWORKS 8
#define WIFI_CONFIG_PRIORITY_AUTO INT32_MIN

typedef struct {
    char ssid[WIFI_CONFIG_SSID_MAX_LEN + 1];
    char password[WIFI_CONFIG_PASSWORD_MAX_LEN + 1];
    bool provisioned;
} wifi_credentials_t;

typedef struct {
    char ssid[WIFI_CONFIG_SSID_MAX_LEN + 1];
    char password[WIFI_CONFIG_PASSWORD_MAX_LEN + 1];
    int32_t priority;
} wifi_saved_network_t;

esp_err_t wifi_config_load(wifi_credentials_t *cfg);
esp_err_t wifi_config_save(const wifi_credentials_t *cfg);
esp_err_t wifi_config_clear(void);
esp_err_t wifi_config_load_saved_networks(wifi_saved_network_t *networks,
                                          size_t max_networks,
                                          size_t *network_count);
esp_err_t wifi_config_upsert_saved_network(const wifi_saved_network_t *network);
esp_err_t wifi_config_remove_saved_network(const char *ssid);

#endif
