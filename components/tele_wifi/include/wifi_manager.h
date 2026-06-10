#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MANAGER_STATE_INIT = 0,
    WIFI_MANAGER_STATE_STA_CONNECTING,
    WIFI_MANAGER_STATE_STA_CONNECTED,
    WIFI_MANAGER_STATE_PROVISIONING_AP,
} wifi_manager_state_t;

typedef enum {
    WIFI_MANAGER_APSTA_ALWAYS_ON = 0,
    WIFI_MANAGER_APSTA_AUTO_TIMEOUT,
    WIFI_MANAGER_APSTA_STA_ONLY,
} wifi_manager_apsta_policy_t;

typedef struct {
    bool wifi_ready;
    bool provisioning_active;
    wifi_manager_state_t state;
    char ip[16];
    char ssid[33];
    char provisioning_ssid[33];
    char last_error[96];
    int rssi;
    int sta_max_retry;
    uint32_t sta_reconnect_attempts;
    uint32_t invalid_transition_count;
    wifi_manager_apsta_policy_t apsta_policy;
    uint32_t apsta_grace_period_s;
    bool apsta_auto_drop_pending;
} wifi_manager_status_t;

typedef struct {
    const char *provisioning_ssid;
    bool force_provisioning;
    int sta_max_retry;
    wifi_manager_apsta_policy_t apsta_policy;
    uint32_t apsta_grace_period_s;
} wifi_manager_config_t;

#define WIFI_MANAGER_MAX_SCAN_RESULTS 16

typedef struct {
    char ssid[33];
    int rssi;
    bool auth_required;
} wifi_manager_network_t;

#define WIFI_MANAGER_MAX_SAVED_NETWORKS 8

typedef struct {
    char ssid[33];
    int32_t priority;
} wifi_manager_saved_network_t;

ESP_EVENT_DECLARE_BASE(WIFI_MANAGER_EVENT);

typedef enum {
    WIFI_MANAGER_EVENT_PROVISIONING_STARTED = 1,
    WIFI_MANAGER_EVENT_PROVISIONING_STOPPED,
    WIFI_MANAGER_EVENT_STA_CONNECTING,
    WIFI_MANAGER_EVENT_STA_CONNECTED,
    WIFI_MANAGER_EVENT_STA_DISCONNECTED,
    WIFI_MANAGER_EVENT_CREDENTIALS_UPDATED,
} wifi_manager_event_id_t;

esp_err_t wifi_manager_start(void);
esp_err_t wifi_manager_start_with_config(const wifi_manager_config_t *config);
esp_err_t wifi_manager_wait_until_ready(TickType_t timeout_ticks);
esp_err_t wifi_manager_apply_wifi_credentials(const char *ssid, const char *password);
esp_err_t wifi_manager_set_provisioning_ssid(const char *ssid);
esp_err_t wifi_manager_reconnect_sta(void);
esp_err_t wifi_manager_set_sta_max_retry(int retry);
esp_err_t wifi_manager_set_apsta_policy(wifi_manager_apsta_policy_t policy,
                                        uint32_t grace_period_s);
esp_err_t wifi_manager_note_portal_activity(void);
esp_err_t wifi_manager_set_high_throughput_mode(bool enabled);
esp_err_t wifi_manager_get_status(wifi_manager_status_t *status);
esp_err_t wifi_manager_scan_networks(wifi_manager_network_t *networks,
                                     size_t max_networks,
                                     size_t *network_count);
esp_err_t wifi_manager_list_saved_networks(wifi_manager_saved_network_t *networks,
                                           size_t max_networks,
                                           size_t *network_count);
esp_err_t wifi_manager_set_saved_network_priority(const char *ssid, int32_t priority);
esp_err_t wifi_manager_remove_saved_network(const char *ssid);

#ifdef __cplusplus
}
#endif

#endif
