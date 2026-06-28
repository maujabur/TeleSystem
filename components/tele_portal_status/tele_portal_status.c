#include "tele_portal_status.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "firmware_version.h"
#include "http_helpers.h"
#include "tele_portal_core.h"
#include "tele_status.h"
#include "time_sync.h"
#include "vbat_monitor.h"
#include "wifi_manager.h"

#ifndef CONFIG_WEB_PORTAL_EXPOSE_NETWORK_IDENTIFIERS
#define CONFIG_WEB_PORTAL_EXPOSE_NETWORK_IDENTIFIERS 0
#endif

#ifndef CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS
#define CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS 0
#endif

#ifndef CONFIG_POWER_GOOD_GPIO_ENABLED
#define CONFIG_POWER_GOOD_GPIO_ENABLED 0
#endif

#ifndef CONFIG_POWER_GOOD_GPIO
#define CONFIG_POWER_GOOD_GPIO 6
#endif

#ifndef CONFIG_POWER_GOOD_ACTIVE_LEVEL
#define CONFIG_POWER_GOOD_ACTIVE_LEVEL 1
#endif

static const char *TAG = "portal-status";

static esp_err_t api_status_get_handler(httpd_req_t *req)
{
    wifi_manager_status_t status;
    vbat_monitor_status_t vbat_status = {0};
    int64_t now_ms = (int64_t)esp_log_timestamp();
    char local_now[32] = {0};
    char utc_now[32] = {0};
    int64_t uptime_ms = now_ms;
    cJSON *json = cJSON_CreateObject();
    cJSON *vbat_json = NULL;
    cJSON *power_good_json = NULL;
    esp_err_t err = wifi_manager_get_status(&status);

    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    tele_portal_core_note_activity();
    cJSON_AddStringToObject(json, "firmware_version", APP_VERSION_STRING);
    cJSON_AddNumberToObject(json, "uptime_ms", (double)uptime_ms);
    cJSON_AddNumberToObject(json, "uptime_seconds", (double)(uptime_ms / 1000));
    power_good_json = cJSON_AddObjectToObject(json, "power_good");
    if (power_good_json) {
        cJSON_AddBoolToObject(power_good_json, "enabled", CONFIG_POWER_GOOD_GPIO_ENABLED);
        cJSON_AddNumberToObject(power_good_json, "gpio", CONFIG_POWER_GOOD_GPIO);
        cJSON_AddNumberToObject(power_good_json, "active_level", CONFIG_POWER_GOOD_ACTIVE_LEVEL);
    }
    cJSON_AddBoolToObject(json, "time_synchronized", time_sync_is_synchronized());
    cJSON_AddStringToObject(json, "timezone", time_sync_timezone());
    if (time_sync_format_local_now(local_now, sizeof(local_now))) {
        cJSON_AddStringToObject(json, "local_time_iso8601", local_now);
    }
    if (time_sync_format_utc_now(utc_now, sizeof(utc_now))) {
        cJSON_AddStringToObject(json, "utc_time_iso8601", utc_now);
    }

    if (err == ESP_OK) {
        cJSON_AddStringToObject(json, "state",
                                status.state == WIFI_MANAGER_STATE_STA_CONNECTED ? "sta_connected" :
                                status.state == WIFI_MANAGER_STATE_STA_CONNECTING ? "sta_connecting" :
                                status.state == WIFI_MANAGER_STATE_PROVISIONING_AP ? "provisioning_ap" : "init");
        cJSON_AddBoolToObject(json, "wifi_ready", status.wifi_ready);
        cJSON_AddBoolToObject(json, "provisioning_active", status.provisioning_active);
        if (CONFIG_WEB_PORTAL_EXPOSE_NETWORK_IDENTIFIERS) {
            cJSON_AddStringToObject(json, "ip", status.ip);
            cJSON_AddStringToObject(json, "ssid", status.ssid);
            cJSON_AddStringToObject(json, "provisioning_ssid", status.provisioning_ssid);
        } else {
            cJSON_AddBoolToObject(json, "network_identifiers_hidden", true);
        }
        cJSON_AddNumberToObject(json, "wifi_rssi", status.rssi);
        cJSON_AddNumberToObject(json, "wifi_sta_max_retry", status.sta_max_retry);
        cJSON_AddNumberToObject(json, "wifi_reconnect_attempts", (double)status.sta_reconnect_attempts);
        cJSON_AddNumberToObject(json, "wifi_invalid_transition_count", (double)status.invalid_transition_count);
        cJSON_AddNumberToObject(json, "wifi_apsta_policy", (double)status.apsta_policy);
        cJSON_AddNumberToObject(json, "wifi_apsta_grace_period_s", (double)status.apsta_grace_period_s);
        cJSON_AddBoolToObject(json, "wifi_apsta_auto_drop_pending", status.apsta_auto_drop_pending);
        cJSON_AddStringToObject(json,
                                "last_error",
                                CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS ? status.last_error : "");
    } else {
        cJSON_AddStringToObject(json,
                                "error",
                                CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS ? esp_err_to_name(err) :
                                "Falha ao carregar status");
    }

    if (vbat_monitor_get_status(&vbat_status) == ESP_OK) {
        vbat_json = cJSON_AddObjectToObject(json, "vbat");
        if (vbat_json) {
            cJSON_AddBoolToObject(vbat_json, "enabled", vbat_status.enabled);
            cJSON_AddBoolToObject(vbat_json, "initialized", vbat_status.initialized);
            cJSON_AddBoolToObject(vbat_json, "calibrated", vbat_status.calibrated);
            cJSON_AddNumberToObject(vbat_json, "gpio", vbat_status.gpio);
            cJSON_AddNumberToObject(vbat_json, "raw_avg", vbat_status.raw_avg);
            cJSON_AddNumberToObject(vbat_json, "gpio_mv", vbat_status.gpio_mv);
            cJSON_AddNumberToObject(vbat_json, "vbat_mv", vbat_status.vbat_mv);
            cJSON_AddNumberToObject(vbat_json,
                                    "last_measurement_ms",
                                    (double)vbat_status.last_measurement_ms);
            cJSON_AddStringToObject(vbat_json,
                                    "last_moment",
                                    vbat_monitor_moment_name(vbat_status.last_moment));
            cJSON_AddNumberToObject(vbat_json,
                                    "measurement_count",
                                    (double)vbat_status.measurement_count);
            cJSON_AddBoolToObject(vbat_json,
                                  "shutdown_enabled",
                                  vbat_status.shutdown_enabled);
            cJSON_AddNumberToObject(vbat_json,
                                    "shutdown_threshold_mv",
                                    vbat_status.shutdown_threshold_mv);
            cJSON_AddNumberToObject(vbat_json,
                                    "shutdown_debounce_ms",
                                    vbat_status.shutdown_debounce_ms);
            cJSON_AddBoolToObject(vbat_json,
                                  "shutdown_countdown_active",
                                  vbat_status.shutdown_countdown_active);
            cJSON_AddNumberToObject(vbat_json,
                                    "shutdown_below_threshold_since_ms",
                                    (double)vbat_status.shutdown_below_threshold_since_ms);
            cJSON_AddNumberToObject(vbat_json,
                                    "shutdown_countdown_elapsed_ms",
                                    vbat_status.shutdown_countdown_active &&
                                    vbat_status.shutdown_below_threshold_since_ms > 0 ?
                                        (double)(now_ms - vbat_status.shutdown_below_threshold_since_ms) :
                                        0.0);
        }
    }

    err = http_helpers_send_json(req, json, err == ESP_OK ? 200 : 500);
    cJSON_Delete(json);
    return err;
}

static esp_err_t api_status_meta_get_handler(httpd_req_t *req)
{
    cJSON *json = cJSON_CreateObject();
    esp_err_t err = ESP_OK;

    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    tele_portal_core_note_activity();
    err = tele_status_add_manifest_to_json(json, TELE_CHANNEL_FLAG_WEB);
    if (err == ESP_OK) {
        err = http_helpers_send_json(req, json, 200);
    }
    cJSON_Delete(json);
    return err;
}

static esp_err_t register_uri_checked(httpd_handle_t server, const httpd_uri_t *route)
{
    esp_err_t err = httpd_register_uri_handler(server, route);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar rota %s: %s", route->uri, esp_err_to_name(err));
    }
    return err;
}

esp_err_t tele_portal_status_register_routes(httpd_handle_t server)
{
    httpd_uri_t api_status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = api_status_get_handler,
    };
    httpd_uri_t api_status_meta = {
        .uri = "/api/status/meta",
        .method = HTTP_GET,
        .handler = api_status_meta_get_handler,
    };

    esp_err_t err = register_uri_checked(server, &api_status);
    if (err == ESP_OK) {
        err = register_uri_checked(server, &api_status_meta);
    }
    return err;
}
