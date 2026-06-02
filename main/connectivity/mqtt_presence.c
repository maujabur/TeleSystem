#include "mqtt_presence.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"

#include "firmware_version.h"
#include "time_sync.h"
#include "acr_analysis_control.h"
#include "acr_config_store.h"
#include "acr_trigger_output.h"
#include "device_config_store.h"
#include "vbat_monitor.h"
#include "wifi_manager.h"

#ifndef CONFIG_MQTT_PRESENCE_ENABLED
#define CONFIG_MQTT_PRESENCE_ENABLED 0
#endif

#ifndef CONFIG_MQTT_BROKER_URI
#define CONFIG_MQTT_BROKER_URI ""
#endif

#ifndef CONFIG_MQTT_USERNAME
#define CONFIG_MQTT_USERNAME ""
#endif

#ifndef CONFIG_MQTT_PASSWORD
#define CONFIG_MQTT_PASSWORD ""
#endif

#ifndef CONFIG_MQTT_TOPIC_NAMESPACE
#define CONFIG_MQTT_TOPIC_NAMESPACE "v1/acr"
#endif

#ifndef CONFIG_MQTT_HEARTBEAT_INTERVAL_S
#define CONFIG_MQTT_HEARTBEAT_INTERVAL_S 60
#endif

#ifndef CONFIG_MQTT_KEEPALIVE_S
#define CONFIG_MQTT_KEEPALIVE_S 60
#endif

#ifndef CONFIG_MQTT_QOS_CRITICAL
#define CONFIG_MQTT_QOS_CRITICAL 1
#endif

#ifndef CONFIG_MQTT_QOS_TELEMETRY
#define CONFIG_MQTT_QOS_TELEMETRY 0
#endif

#define MQTT_TOPIC_BUF_SIZE 128
#define MQTT_DEVICE_ID_SIZE 64
#define MQTT_TS_BUF_SIZE 40
#define MQTT_JSON_BUF_SIZE 512
#define MQTT_CMD_ID_SIZE 64
#define MQTT_CMD_DEDUP_WINDOW 16
#define MQTT_SESSION_ID_SIZE 48

static const char *TAG = "mqtt-presence";

static bool s_started;
static bool s_client_started;
static bool s_connected;
static esp_mqtt_client_handle_t s_client;
static TaskHandle_t s_heartbeat_task;
static bool s_wifi_event_registered;
static uint32_t s_last_wait_log_ms;
static uint32_t s_heartbeat_interval_s;
static bool s_restart_requested;
static uint32_t s_restart_at_ms;
static char s_recent_mutating_cmd_ids[MQTT_CMD_DEDUP_WINDOW][MQTT_CMD_ID_SIZE];
static size_t s_recent_mutating_cmd_index;

static char s_device_id[MQTT_DEVICE_ID_SIZE];
static char s_session_id[MQTT_SESSION_ID_SIZE];
static char s_broker_host[96];
static uint32_t s_broker_port;
static esp_mqtt_transport_t s_broker_transport;
static char s_lwt_payload[MQTT_JSON_BUF_SIZE];
static char s_topic_status[MQTT_TOPIC_BUF_SIZE];
static char s_topic_heartbeat[MQTT_TOPIC_BUF_SIZE];
static char s_topic_state[MQTT_TOPIC_BUF_SIZE];
static char s_topic_event[MQTT_TOPIC_BUF_SIZE];
static char s_topic_cmd_in[MQTT_TOPIC_BUF_SIZE];
static char s_topic_cmd_out[MQTT_TOPIC_BUF_SIZE];

static esp_err_t mqtt_presence_start_client_if_needed(void);
static void mqtt_presence_build_timestamp(char *buffer, size_t buffer_len);
static void mqtt_presence_ensure_session_id(void);
static const char *wifi_state_name(wifi_manager_state_t state);
static int mqtt_publish_if_connected(const char *topic, const char *payload, int qos, int retain);
static void mqtt_presence_publish_command_reply(const char *cmd_id,
                                                bool ok,
                                                const char *error,
                                                const cJSON *result);

static cJSON *mqtt_presence_build_state_result(void);
static cJSON *mqtt_presence_build_settings_result(void);
static void mqtt_presence_handle_command_payload(const char *payload);

static void mqtt_presence_add_common_fields(cJSON *root, const char *ts)
{
    if (!root) {
        return;
    }

    cJSON_AddStringToObject(root, "device_id", s_device_id);
    cJSON_AddStringToObject(root, "fw", APP_VERSION_STRING);
    cJSON_AddStringToObject(root, "session_id", s_session_id[0] != '\0' ? s_session_id : "unknown");
    if (ts) {
        cJSON_AddStringToObject(root, "ts", ts);
    }
}

static bool mqtt_presence_cmd_id_seen(const char *cmd_id)
{
    if (!cmd_id || cmd_id[0] == '\0') {
        return false;
    }

    for (size_t index = 0; index < MQTT_CMD_DEDUP_WINDOW; ++index) {
        if (strncmp(s_recent_mutating_cmd_ids[index], cmd_id, MQTT_CMD_ID_SIZE) == 0) {
            return true;
        }
    }

    return false;
}

static void mqtt_presence_cmd_id_remember(const char *cmd_id)
{
    if (!cmd_id || cmd_id[0] == '\0') {
        return;
    }

    snprintf(s_recent_mutating_cmd_ids[s_recent_mutating_cmd_index],
             MQTT_CMD_ID_SIZE,
             "%s",
             cmd_id);
    s_recent_mutating_cmd_index = (s_recent_mutating_cmd_index + 1) % MQTT_CMD_DEDUP_WINDOW;
}

static bool mqtt_presence_reject_duplicate_mutating_command(const char *cmd_id)
{
    cJSON *result = NULL;

    if (!mqtt_presence_cmd_id_seen(cmd_id)) {
        return false;
    }

    result = cJSON_CreateObject();
    if (result) {
        cJSON_AddBoolToObject(result, "duplicate", true);
        cJSON_AddBoolToObject(result, "executed", false);
    }
    mqtt_presence_publish_command_reply(cmd_id, true, NULL, result);
    cJSON_Delete(result);
    ESP_LOGW(TAG, "Comando mutavel duplicado ignorado | cmd_id=%s", cmd_id);
    return true;
}

static void mqtt_presence_schedule_restart(uint32_t delay_ms)
{
    uint32_t bounded_delay_ms = delay_ms;

    if (bounded_delay_ms < 100) {
        bounded_delay_ms = 100;
    }
    if (bounded_delay_ms > 10000) {
        bounded_delay_ms = 10000;
    }

    s_restart_requested = true;
    s_restart_at_ms = esp_log_timestamp() + bounded_delay_ms;
    ESP_LOGW(TAG, "Reboot agendado via MQTT em %u ms", (unsigned)bounded_delay_ms);
}

static void mqtt_presence_publish_command_reply(const char *cmd_id,
                                                bool ok,
                                                const char *error,
                                                const cJSON *result)
{
    char ts[MQTT_TS_BUF_SIZE] = {0};
    cJSON *root = cJSON_CreateObject();
    char *payload_text = NULL;

    if (!root) {
        return;
    }

    mqtt_presence_build_timestamp(ts, sizeof(ts));

    mqtt_presence_add_common_fields(root, ts);
    cJSON_AddStringToObject(root, "cmd_id", cmd_id ? cmd_id : "unknown");
    cJSON_AddBoolToObject(root, "ok", ok);
    if (error && error[0] != '\0') {
        cJSON_AddStringToObject(root, "error", error);
    }

    if (result) {
        cJSON *dup = cJSON_Duplicate((cJSON *)result, 1);
        if (dup) {
            cJSON_AddItemToObject(root, "result", dup);
        }
    }

    payload_text = cJSON_PrintUnformatted(root);
    if (payload_text) {
        mqtt_publish_if_connected(s_topic_cmd_out, payload_text, CONFIG_MQTT_QOS_CRITICAL, 0);
        cJSON_free(payload_text);
    }
    cJSON_Delete(root);
}

static cJSON *mqtt_presence_build_state_result(void)
{
    wifi_manager_status_t wifi_status = {0};
    vbat_monitor_status_t vbat_status = {0};
    cJSON *result = cJSON_CreateObject();

    if (!result) {
        return NULL;
    }

    if (wifi_manager_get_status(&wifi_status) != ESP_OK) {
        cJSON_AddStringToObject(result, "wifi_state", "unknown");
        cJSON_AddBoolToObject(result, "wifi_ready", false);
    } else {
        cJSON_AddStringToObject(result, "wifi_state", wifi_state_name(wifi_status.state));
        cJSON_AddBoolToObject(result, "wifi_ready", wifi_status.wifi_ready);
        cJSON_AddStringToObject(result, "ssid", wifi_status.ssid);
        cJSON_AddStringToObject(result, "ip", wifi_status.ip);
        cJSON_AddNumberToObject(result, "rssi", wifi_status.rssi);
    }

    (void)vbat_monitor_get_status(&vbat_status);
    cJSON_AddNumberToObject(result, "vbat_mv", vbat_status.vbat_mv);
    cJSON_AddNumberToObject(result, "heap_free", (double)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    cJSON_AddNumberToObject(result, "uptime_s", (double)(esp_log_timestamp() / 1000));
    cJSON_AddNumberToObject(result, "heartbeat_interval_s", (double)s_heartbeat_interval_s);
    cJSON_AddBoolToObject(result, "time_synchronized", time_sync_is_synchronized());

    return result;
}

static cJSON *mqtt_presence_build_settings_result(void)
{
    cJSON *result = cJSON_CreateObject();
    acr_analysis_control_config_t control = {0};
    acr_trigger_output_status_t trigger = {0};
    acr_config_public_info_t acr_info = {0};
    char provisioning_ssid[DEVICE_CONFIG_PROVISIONING_SSID_BUFFER_SIZE] = {0};
    uint8_t sta_max_retry = 0;
    device_config_apsta_policy_t apsta_policy = DEVICE_CONFIG_APSTA_AUTO_TIMEOUT;
    uint32_t apsta_grace_period_s = 0;
    cJSON *section = NULL;

    if (!result) {
        return NULL;
    }

    section = cJSON_AddObjectToObject(result, "acr_control");
    if (section) {
        if (acr_analysis_control_get_config(&control) == ESP_OK) {
            cJSON_AddBoolToObject(section, "automatic_enabled", control.automatic_enabled);
            cJSON_AddNumberToObject(section, "automatic_interval_ms", control.automatic_interval_ms);
            cJSON_AddNumberToObject(section, "capture_duration_seconds", control.capture_duration_seconds);
            cJSON_AddNumberToObject(section, "digital_gain", control.digital_gain);
            cJSON_AddNumberToObject(section, "silence_threshold_rms", control.silence_threshold_rms);
            cJSON_AddNumberToObject(section, "silence_hysteresis_rms", control.silence_hysteresis_rms);
            cJSON_AddNumberToObject(section, "min_active_ms", control.min_active_ms);
            cJSON_AddNumberToObject(section, "trigger_mode", control.trigger_mode);
            cJSON_AddNumberToObject(section, "ai_probability_threshold", control.ai_probability_threshold);
        } else {
            cJSON_AddStringToObject(section, "error", "acr_control_unavailable");
        }
    }

    section = cJSON_AddObjectToObject(result, "trigger_output");
    if (section) {
        if (acr_trigger_output_get_status(&trigger) == ESP_OK) {
            cJSON_AddBoolToObject(section, "enabled", trigger.config.enabled);
            cJSON_AddNumberToObject(section, "gpio", trigger.config.gpio);
            cJSON_AddNumberToObject(section, "active_level", trigger.config.active_level);
            cJSON_AddNumberToObject(section, "pulse_ms", trigger.config.pulse_ms);
        } else {
            cJSON_AddStringToObject(section, "error", "trigger_output_unavailable");
        }
    }

    section = cJSON_AddObjectToObject(result, "acr_cloud");
    if (section) {
        if (acr_config_store_get_public_info(&acr_info) == ESP_OK) {
            cJSON_AddStringToObject(section, "region", acr_info.region);
            cJSON_AddStringToObject(section, "container_id", acr_info.container_id);
            cJSON_AddStringToObject(section, "upload_prefix", acr_info.upload_prefix);
            cJSON_AddBoolToObject(section, "token_configured", acr_info.token_configured);
            cJSON_AddBoolToObject(section, "root_cert_configured", acr_info.root_cert_configured);
        } else {
            cJSON_AddStringToObject(section, "error", "acr_cloud_unavailable");
        }
    }

    section = cJSON_AddObjectToObject(result, "device_connectivity");
    if (section) {
        if (device_config_store_load_provisioning_ssid(provisioning_ssid, sizeof(provisioning_ssid)) == ESP_OK) {
            cJSON_AddStringToObject(section, "provisioning_ssid", provisioning_ssid);
        }
        if (device_config_store_load_sta_max_retry(&sta_max_retry) == ESP_OK) {
            cJSON_AddNumberToObject(section, "sta_max_retry", sta_max_retry);
        }
        if (device_config_store_load_apsta_policy(&apsta_policy, &apsta_grace_period_s) == ESP_OK) {
            cJSON_AddNumberToObject(section, "apsta_policy", (int)apsta_policy);
            cJSON_AddNumberToObject(section, "apsta_grace_period_s", (double)apsta_grace_period_s);
        }
    }

    section = cJSON_AddObjectToObject(result, "mqtt");
    if (section) {
        cJSON_AddNumberToObject(section, "heartbeat_interval_s", (double)s_heartbeat_interval_s);
    }

    return result;
}

static void mqtt_presence_handle_command_payload(const char *payload)
{
    cJSON *root = NULL;
    cJSON *cmd_id_item = NULL;
    cJSON *name_item = NULL;
    const char *cmd_id = "unknown";
    const char *cmd_name = NULL;

    if (!payload) {
        return;
    }

    root = cJSON_Parse(payload);
    if (!root) {
        mqtt_presence_publish_command_reply("unknown", false, "invalid_json", NULL);
        return;
    }

    cmd_id_item = cJSON_GetObjectItemCaseSensitive(root, "cmd_id");
    if (cJSON_IsString(cmd_id_item) && cmd_id_item->valuestring) {
        cmd_id = cmd_id_item->valuestring;
    }

    name_item = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (!cJSON_IsString(name_item) || !name_item->valuestring) {
        cJSON_Delete(root);
        mqtt_presence_publish_command_reply(cmd_id, false, "missing_name", NULL);
        return;
    }
    cmd_name = name_item->valuestring;

    if (strcmp(cmd_name, "ping") == 0) {
        cJSON *result = cJSON_CreateObject();
        if (result) {
            cJSON_AddBoolToObject(result, "pong", true);
            cJSON_AddNumberToObject(result, "uptime_s", (double)(esp_log_timestamp() / 1000));
            cJSON_AddBoolToObject(result, "time_synchronized", time_sync_is_synchronized());
        }
        mqtt_presence_publish_command_reply(cmd_id, true, NULL, result);
        cJSON_Delete(result);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(cmd_name, "get_state") == 0) {
        cJSON *result = mqtt_presence_build_state_result();
        mqtt_presence_publish_command_reply(cmd_id, true, NULL, result);
        cJSON_Delete(result);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(cmd_name, "get_settings") == 0) {
        cJSON *result = mqtt_presence_build_settings_result();
        mqtt_presence_publish_command_reply(cmd_id, true, NULL, result);
        cJSON_Delete(result);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(cmd_name, "set_heartbeat_interval") == 0) {
        cJSON *args = cJSON_GetObjectItemCaseSensitive(root, "args");
        cJSON *interval = args ? cJSON_GetObjectItemCaseSensitive(args, "heartbeat_interval_s") : NULL;

        if (mqtt_presence_reject_duplicate_mutating_command(cmd_id)) {
            cJSON_Delete(root);
            return;
        }

        if (!cJSON_IsNumber(interval)) {
            cJSON_Delete(root);
            mqtt_presence_publish_command_reply(cmd_id, false, "missing_heartbeat_interval_s", NULL);
            return;
        }

        if (interval->valuedouble < 15 || interval->valuedouble > 3600) {
            cJSON_Delete(root);
            mqtt_presence_publish_command_reply(cmd_id, false, "heartbeat_interval_out_of_range", NULL);
            return;
        }

        s_heartbeat_interval_s = (uint32_t)interval->valuedouble;
        mqtt_presence_cmd_id_remember(cmd_id);

        cJSON *result = cJSON_CreateObject();
        if (result) {
            cJSON_AddNumberToObject(result, "heartbeat_interval_s", (double)s_heartbeat_interval_s);
        }
        mqtt_presence_publish_command_reply(cmd_id, true, NULL, result);
        cJSON_Delete(result);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(cmd_name, "set_settings") == 0) {
        cJSON *args = cJSON_GetObjectItemCaseSensitive(root, "args");
        bool updated = false;

        if (mqtt_presence_reject_duplicate_mutating_command(cmd_id)) {
            cJSON_Delete(root);
            return;
        }

        if (!cJSON_IsObject(args)) {
            cJSON_Delete(root);
            mqtt_presence_publish_command_reply(cmd_id, false, "missing_args_object", NULL);
            return;
        }

        cJSON *acr_control = cJSON_GetObjectItemCaseSensitive(args, "acr_control");
        if (cJSON_IsObject(acr_control)) {
            acr_analysis_control_config_t config = {0};
            esp_err_t err = acr_analysis_control_get_config(&config);
            if (err != ESP_OK) {
                cJSON_Delete(root);
                mqtt_presence_publish_command_reply(cmd_id, false, "acr_control_unavailable", NULL);
                return;
            }

            cJSON *item = NULL;
            item = cJSON_GetObjectItemCaseSensitive(acr_control, "automatic_enabled");
            if (cJSON_IsBool(item)) {
                config.automatic_enabled = cJSON_IsTrue(item);
            }
            item = cJSON_GetObjectItemCaseSensitive(acr_control, "automatic_interval_ms");
            if (cJSON_IsNumber(item)) {
                if (item->valuedouble < 0 || item->valuedouble > 3600000) {
                    cJSON_Delete(root);
                    mqtt_presence_publish_command_reply(cmd_id, false, "automatic_interval_ms_out_of_range", NULL);
                    return;
                }
                config.automatic_interval_ms = (uint32_t)item->valuedouble;
            }
            item = cJSON_GetObjectItemCaseSensitive(acr_control, "capture_duration_seconds");
            if (cJSON_IsNumber(item)) {
                if (item->valuedouble < 1 || item->valuedouble > 30) {
                    cJSON_Delete(root);
                    mqtt_presence_publish_command_reply(cmd_id, false, "capture_duration_seconds_out_of_range", NULL);
                    return;
                }
                config.capture_duration_seconds = (uint32_t)item->valuedouble;
            }
            item = cJSON_GetObjectItemCaseSensitive(acr_control, "digital_gain");
            if (cJSON_IsNumber(item)) {
                if (item->valuedouble < 0.25 || item->valuedouble > 16.0) {
                    cJSON_Delete(root);
                    mqtt_presence_publish_command_reply(cmd_id, false, "digital_gain_out_of_range", NULL);
                    return;
                }
                config.digital_gain = item->valuedouble;
            }
            item = cJSON_GetObjectItemCaseSensitive(acr_control, "silence_threshold_rms");
            if (cJSON_IsNumber(item)) {
                if (item->valuedouble < 0 || item->valuedouble > 32767) {
                    cJSON_Delete(root);
                    mqtt_presence_publish_command_reply(cmd_id, false, "silence_threshold_rms_out_of_range", NULL);
                    return;
                }
                config.silence_threshold_rms = (uint32_t)item->valuedouble;
            }
            item = cJSON_GetObjectItemCaseSensitive(acr_control, "silence_hysteresis_rms");
            if (cJSON_IsNumber(item)) {
                if (item->valuedouble < 0 || item->valuedouble > 32767) {
                    cJSON_Delete(root);
                    mqtt_presence_publish_command_reply(cmd_id, false, "silence_hysteresis_rms_out_of_range", NULL);
                    return;
                }
                config.silence_hysteresis_rms = (uint32_t)item->valuedouble;
            }
            item = cJSON_GetObjectItemCaseSensitive(acr_control, "min_active_ms");
            if (cJSON_IsNumber(item)) {
                if (item->valuedouble < 0 || item->valuedouble > 30000) {
                    cJSON_Delete(root);
                    mqtt_presence_publish_command_reply(cmd_id, false, "min_active_ms_out_of_range", NULL);
                    return;
                }
                config.min_active_ms = (uint32_t)item->valuedouble;
            }
            item = cJSON_GetObjectItemCaseSensitive(acr_control, "trigger_mode");
            if (cJSON_IsNumber(item)) {
                if (item->valueint < 0 || item->valueint > 3) {
                    cJSON_Delete(root);
                    mqtt_presence_publish_command_reply(cmd_id, false, "trigger_mode_out_of_range", NULL);
                    return;
                }
                config.trigger_mode = (acr_trigger_mode_t)item->valueint;
            }
            item = cJSON_GetObjectItemCaseSensitive(acr_control, "ai_probability_threshold");
            if (cJSON_IsNumber(item)) {
                if (item->valuedouble < 0 || item->valuedouble > 100) {
                    cJSON_Delete(root);
                    mqtt_presence_publish_command_reply(cmd_id, false, "ai_probability_threshold_out_of_range", NULL);
                    return;
                }
                config.ai_probability_threshold = item->valuedouble;
            }

            err = acr_analysis_control_save_config(&config);
            if (err != ESP_OK) {
                cJSON_Delete(root);
                mqtt_presence_publish_command_reply(cmd_id, false, "acr_control_save_failed", NULL);
                return;
            }
            updated = true;
        }

        cJSON *trigger_output = cJSON_GetObjectItemCaseSensitive(args, "trigger_output");
        if (cJSON_IsObject(trigger_output)) {
            acr_trigger_output_status_t status = {0};
            acr_trigger_output_config_t config = {0};
            esp_err_t err = acr_trigger_output_get_status(&status);
            if (err != ESP_OK) {
                cJSON_Delete(root);
                mqtt_presence_publish_command_reply(cmd_id, false, "trigger_output_unavailable", NULL);
                return;
            }

            config = status.config;
            cJSON *item = NULL;
            item = cJSON_GetObjectItemCaseSensitive(trigger_output, "enabled");
            if (cJSON_IsBool(item)) {
                config.enabled = cJSON_IsTrue(item);
            }
            item = cJSON_GetObjectItemCaseSensitive(trigger_output, "gpio");
            if (cJSON_IsNumber(item)) {
                config.gpio = item->valueint;
            }
            item = cJSON_GetObjectItemCaseSensitive(trigger_output, "active_level");
            if (cJSON_IsNumber(item)) {
                config.active_level = item->valueint;
            }
            item = cJSON_GetObjectItemCaseSensitive(trigger_output, "pulse_ms");
            if (cJSON_IsNumber(item)) {
                config.pulse_ms = (uint32_t)item->valuedouble;
            }

            err = acr_trigger_output_save_config(&config);
            if (err != ESP_OK) {
                cJSON_Delete(root);
                mqtt_presence_publish_command_reply(cmd_id, false, "trigger_output_save_failed", NULL);
                return;
            }
            updated = true;
        }

        cJSON *acr_cloud = cJSON_GetObjectItemCaseSensitive(args, "acr_cloud");
        if (cJSON_IsObject(acr_cloud)) {
            cJSON *item = NULL;
            esp_err_t err = ESP_OK;

            item = cJSON_GetObjectItemCaseSensitive(acr_cloud, "region");
            if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
                err = acr_config_store_save_region(item->valuestring);
                if (err != ESP_OK) {
                    cJSON_Delete(root);
                    mqtt_presence_publish_command_reply(cmd_id, false, "acr_region_save_failed", NULL);
                    return;
                }
                updated = true;
            }

            item = cJSON_GetObjectItemCaseSensitive(acr_cloud, "container_id");
            if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
                err = acr_config_store_save_container_id(item->valuestring);
                if (err != ESP_OK) {
                    cJSON_Delete(root);
                    mqtt_presence_publish_command_reply(cmd_id, false, "acr_container_id_save_failed", NULL);
                    return;
                }
                updated = true;
            }

            item = cJSON_GetObjectItemCaseSensitive(acr_cloud, "upload_prefix");
            if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
                err = acr_config_store_save_upload_prefix(item->valuestring);
                if (err != ESP_OK) {
                    cJSON_Delete(root);
                    mqtt_presence_publish_command_reply(cmd_id, false, "acr_upload_prefix_save_failed", NULL);
                    return;
                }
                updated = true;
            }

            item = cJSON_GetObjectItemCaseSensitive(acr_cloud, "bearer_token");
            if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
                err = acr_config_store_save_bearer_token(item->valuestring);
                if (err != ESP_OK) {
                    cJSON_Delete(root);
                    mqtt_presence_publish_command_reply(cmd_id, false, "acr_bearer_token_save_failed", NULL);
                    return;
                }
                updated = true;
            }
        }

        cJSON *device_connectivity = cJSON_GetObjectItemCaseSensitive(args, "device_connectivity");
        if (cJSON_IsObject(device_connectivity)) {
            cJSON *item = NULL;
            esp_err_t err = ESP_OK;

            item = cJSON_GetObjectItemCaseSensitive(device_connectivity, "provisioning_ssid");
            if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
                err = device_config_store_save_provisioning_ssid(item->valuestring);
                if (err != ESP_OK) {
                    cJSON_Delete(root);
                    mqtt_presence_publish_command_reply(cmd_id, false, "provisioning_ssid_save_failed", NULL);
                    return;
                }
                err = wifi_manager_set_provisioning_ssid(item->valuestring);
                if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                    cJSON_Delete(root);
                    mqtt_presence_publish_command_reply(cmd_id, false, "provisioning_ssid_apply_failed", NULL);
                    return;
                }
                updated = true;
            }

            item = cJSON_GetObjectItemCaseSensitive(device_connectivity, "sta_max_retry");
            if (cJSON_IsNumber(item)) {
                int retry = item->valueint;
                if (retry < DEVICE_CONFIG_STA_MAX_RETRY_MIN || retry > DEVICE_CONFIG_STA_MAX_RETRY_MAX) {
                    cJSON_Delete(root);
                    mqtt_presence_publish_command_reply(cmd_id, false, "sta_max_retry_out_of_range", NULL);
                    return;
                }

                err = device_config_store_save_sta_max_retry((uint8_t)retry);
                if (err != ESP_OK) {
                    cJSON_Delete(root);
                    mqtt_presence_publish_command_reply(cmd_id, false, "sta_max_retry_save_failed", NULL);
                    return;
                }
                err = wifi_manager_set_sta_max_retry(retry);
                if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                    cJSON_Delete(root);
                    mqtt_presence_publish_command_reply(cmd_id, false, "sta_max_retry_apply_failed", NULL);
                    return;
                }
                updated = true;
            }

            cJSON *policy_item = cJSON_GetObjectItemCaseSensitive(device_connectivity, "apsta_policy");
            cJSON *grace_item = cJSON_GetObjectItemCaseSensitive(device_connectivity, "apsta_grace_period_s");
            if (policy_item || grace_item) {
                if (!cJSON_IsNumber(policy_item) || !cJSON_IsNumber(grace_item)) {
                    cJSON_Delete(root);
                    mqtt_presence_publish_command_reply(cmd_id, false, "apsta_policy_and_grace_required", NULL);
                    return;
                }

                int policy = policy_item->valueint;
                uint32_t grace_period_s = (uint32_t)grace_item->valuedouble;
                if (policy < DEVICE_CONFIG_APSTA_ALWAYS_ON ||
                    policy > DEVICE_CONFIG_APSTA_STA_ONLY ||
                    grace_period_s < DEVICE_CONFIG_APSTA_GRACE_PERIOD_S_MIN ||
                    grace_period_s > DEVICE_CONFIG_APSTA_GRACE_PERIOD_S_MAX) {
                    cJSON_Delete(root);
                    mqtt_presence_publish_command_reply(cmd_id, false, "apsta_config_out_of_range", NULL);
                    return;
                }

                err = device_config_store_save_apsta_policy((device_config_apsta_policy_t)policy,
                                                            grace_period_s);
                if (err != ESP_OK) {
                    cJSON_Delete(root);
                    mqtt_presence_publish_command_reply(cmd_id, false, "apsta_config_save_failed", NULL);
                    return;
                }
                err = wifi_manager_set_apsta_policy((wifi_manager_apsta_policy_t)policy, grace_period_s);
                if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                    cJSON_Delete(root);
                    mqtt_presence_publish_command_reply(cmd_id, false, "apsta_config_apply_failed", NULL);
                    return;
                }
                updated = true;
            }
        }

        if (!updated) {
            cJSON_Delete(root);
            mqtt_presence_publish_command_reply(cmd_id, false, "no_supported_settings_in_args", NULL);
            return;
        }

        mqtt_presence_cmd_id_remember(cmd_id);
        cJSON *result = mqtt_presence_build_settings_result();
        mqtt_presence_publish_command_reply(cmd_id, true, NULL, result);
        cJSON_Delete(result);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(cmd_name, "apply_and_reboot") == 0) {
        cJSON *args = cJSON_GetObjectItemCaseSensitive(root, "args");
        cJSON *delay_item = cJSON_IsObject(args) ? cJSON_GetObjectItemCaseSensitive(args, "delay_ms") : NULL;
        uint32_t delay_ms = 800;
        cJSON *result = cJSON_CreateObject();

        if (mqtt_presence_reject_duplicate_mutating_command(cmd_id)) {
            cJSON_Delete(root);
            return;
        }

        if (cJSON_IsNumber(delay_item) && delay_item->valuedouble > 0) {
            delay_ms = (uint32_t)delay_item->valuedouble;
        }

        mqtt_presence_schedule_restart(delay_ms);
        mqtt_presence_cmd_id_remember(cmd_id);

        if (result) {
            uint32_t bounded_delay_ms = delay_ms;
            if (bounded_delay_ms < 100) {
                bounded_delay_ms = 100;
            }
            if (bounded_delay_ms > 10000) {
                bounded_delay_ms = 10000;
            }
            cJSON_AddBoolToObject(result, "restart_scheduled", true);
            cJSON_AddNumberToObject(result, "restart_delay_ms", (double)bounded_delay_ms);
        }

        mqtt_presence_publish_command_reply(cmd_id, true, NULL, result);
        cJSON_Delete(result);
        cJSON_Delete(root);
        return;
    }

    cJSON_Delete(root);
    mqtt_presence_publish_command_reply(cmd_id, false, "unsupported_command", NULL);
}

static void mqtt_presence_normalize_id_prefix(char *dst, size_t dst_len, const char *src)
{
    size_t di = 0;

    if (!dst || dst_len == 0) {
        return;
    }

    dst[0] = '\0';

    if (!src || src[0] == '\0') {
        snprintf(dst, dst_len, "ACR");
        return;
    }

    for (size_t si = 0; src[si] != '\0' && di + 1 < dst_len; ++si) {
        unsigned char c = (unsigned char)src[si];
        if (isalnum(c) || c == '-' || c == '_') {
            dst[di++] = (char)c;
            continue;
        }

        if (c == ' ' || c == '.') {
            dst[di++] = '-';
        }
    }

    if (di == 0) {
        snprintf(dst, dst_len, "ACR");
        return;
    }

    dst[di] = '\0';
}

static bool mqtt_presence_parse_broker_uri(const char *uri,
                                           char *host,
                                           size_t host_len,
                                           uint32_t *port,
                                           esp_mqtt_transport_t *transport)
{
    const char *scheme = NULL;
    const char *host_start = NULL;
    const char *host_end = NULL;
    size_t host_size = 0;
    uint32_t parsed_port = 0;

    if (!uri || !host || host_len == 0 || !port || !transport) {
        return false;
    }

    if (strncmp(uri, "mqtts://", 8) == 0) {
        scheme = "mqtts";
        host_start = uri + 8;
        *transport = MQTT_TRANSPORT_OVER_SSL;
        parsed_port = 8883;
    } else if (strncmp(uri, "mqtt://", 7) == 0) {
        scheme = "mqtt";
        host_start = uri + 7;
        *transport = MQTT_TRANSPORT_OVER_TCP;
        parsed_port = 1883;
    } else {
        return false;
    }

    if (!host_start || *host_start == '\0') {
        return false;
    }

    host_end = host_start;
    while (*host_end != '\0' && *host_end != ':' && *host_end != '/') {
        host_end++;
    }

    host_size = (size_t)(host_end - host_start);
    if (host_size == 0 || host_size >= host_len) {
        return false;
    }

    memcpy(host, host_start, host_size);
    host[host_size] = '\0';

    if (*host_end == ':') {
        const char *port_start = host_end + 1;
        parsed_port = 0;
        while (*port_start >= '0' && *port_start <= '9') {
            parsed_port = (parsed_port * 10u) + (uint32_t)(*port_start - '0');
            port_start++;
        }
        if (parsed_port == 0) {
            return false;
        }
    }

    *port = parsed_port;
    (void)scheme;
    return true;
}

static void mqtt_presence_build_timestamp(char *buffer, size_t buffer_len)
{
    if (!buffer || buffer_len == 0) {
        return;
    }

    if (!time_sync_format_utc_now(buffer, buffer_len)) {
        snprintf(buffer, buffer_len, "1970-01-01T00:00:00Z");
    }
}

static void mqtt_presence_ensure_session_id(void)
{
    uint8_t mac[6] = {0};
    esp_err_t mac_err = esp_read_mac(mac, ESP_MAC_WIFI_STA);

    if (s_session_id[0] != '\0') {
        return;
    }

    if (time_sync_is_synchronized()) {
        time_t now = 0;
        struct tm now_tm = {0};

        time(&now);
        if (now > 0 && gmtime_r(&now, &now_tm) != NULL && (now_tm.tm_year + 1900) >= 2024) {
            snprintf(s_session_id,
                     sizeof(s_session_id),
                     "%04d%02d%02dT%02d%02d%02dZ-%02X%02X%02X",
                     now_tm.tm_year + 1900,
                     now_tm.tm_mon + 1,
                     now_tm.tm_mday,
                     now_tm.tm_hour,
                     now_tm.tm_min,
                     now_tm.tm_sec,
                     mac_err == ESP_OK ? (unsigned)mac[3] : 0u,
                     mac_err == ESP_OK ? (unsigned)mac[4] : 0u,
                     mac_err == ESP_OK ? (unsigned)mac[5] : 0u);
            return;
        }
    }

    snprintf(s_session_id,
             sizeof(s_session_id),
             "boot-%08" PRIX32 "-%02X%02X%02X",
             esp_log_timestamp(),
             mac_err == ESP_OK ? (unsigned)mac[3] : 0u,
             mac_err == ESP_OK ? (unsigned)mac[4] : 0u,
             mac_err == ESP_OK ? (unsigned)mac[5] : 0u);
}

static const char *wifi_state_name(wifi_manager_state_t state)
{
    switch (state) {
    case WIFI_MANAGER_STATE_INIT:
        return "init";
    case WIFI_MANAGER_STATE_STA_CONNECTING:
        return "sta_connecting";
    case WIFI_MANAGER_STATE_STA_CONNECTED:
        return "sta_connected";
    case WIFI_MANAGER_STATE_PROVISIONING_AP:
        return "provisioning_ap";
    default:
        return "unknown";
    }
}

static void json_escape_copy(char *dst, size_t dst_len, const char *src)
{
    size_t di = 0;

    if (!dst || dst_len == 0) {
        return;
    }

    dst[0] = '\0';

    if (!src) {
        return;
    }

    for (size_t si = 0; src[si] != '\0' && di + 1 < dst_len; ++si) {
        char c = src[si];
        if ((c == '\\' || c == '"') && di + 2 < dst_len) {
            dst[di++] = '\\';
            dst[di++] = c;
        } else if ((unsigned char)c >= 0x20) {
            dst[di++] = c;
        }
    }

    dst[di] = '\0';
}

static int mqtt_publish_if_connected(const char *topic, const char *payload, int qos, int retain)
{
    if (!s_connected || !s_client || !topic || !payload) {
        return -1;
    }

    return esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain);
}

static void mqtt_presence_publish_status(const char *status_text, const char *reason)
{
    char ts[MQTT_TS_BUF_SIZE] = {0};
    char payload[MQTT_JSON_BUF_SIZE] = {0};

    mqtt_presence_build_timestamp(ts, sizeof(ts));

    snprintf(payload,
             sizeof(payload),
             "{\"device_id\":\"%s\",\"fw\":\"%s\",\"session_id\":\"%s\",\"status\":\"%s\",\"ts\":\"%s\",\"reason\":\"%s\"}",
             s_device_id,
             APP_VERSION_STRING,
             s_session_id,
             status_text ? status_text : "unknown",
             ts,
             reason ? reason : "runtime");

    mqtt_publish_if_connected(s_topic_status, payload, CONFIG_MQTT_QOS_CRITICAL, 1);
}

static void mqtt_presence_publish_state_snapshot(void)
{
    wifi_manager_status_t wifi_status = {0};
    char ts[MQTT_TS_BUF_SIZE] = {0};
    char ssid_escaped[80] = {0};
    char payload[MQTT_JSON_BUF_SIZE] = {0};

    if (wifi_manager_get_status(&wifi_status) != ESP_OK) {
        return;
    }

    mqtt_presence_build_timestamp(ts, sizeof(ts));
    json_escape_copy(ssid_escaped, sizeof(ssid_escaped), wifi_status.ssid);

    snprintf(payload,
             sizeof(payload),
             "{\"device_id\":\"%s\",\"fw\":\"%s\",\"session_id\":\"%s\",\"ts\":\"%s\",\"wifi_state\":\"%s\",\"wifi_ready\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d}",
             s_device_id,
             APP_VERSION_STRING,
             s_session_id,
             ts,
             wifi_state_name(wifi_status.state),
             wifi_status.wifi_ready ? "true" : "false",
             ssid_escaped,
             wifi_status.ip,
             wifi_status.rssi);

    mqtt_publish_if_connected(s_topic_state, payload, CONFIG_MQTT_QOS_CRITICAL, 1);
}

static void mqtt_presence_publish_heartbeat(void)
{
    wifi_manager_status_t wifi_status = {0};
    vbat_monitor_status_t vbat_status = {0};
    char ts[MQTT_TS_BUF_SIZE] = {0};
    char payload[MQTT_JSON_BUF_SIZE] = {0};

    if (wifi_manager_get_status(&wifi_status) != ESP_OK) {
        return;
    }

    (void)vbat_monitor_get_status(&vbat_status);

    mqtt_presence_build_timestamp(ts, sizeof(ts));

    snprintf(payload,
             sizeof(payload),
             "{\"device_id\":\"%s\",\"fw\":\"%s\",\"session_id\":\"%s\",\"ts\":\"%s\",\"uptime_s\":%" PRIu64 ",\"rssi\":%d,\"heap_free\":%u,\"vbat_mv\":%d,\"wifi_state\":\"%s\"}",
             s_device_id,
             APP_VERSION_STRING,
             s_session_id,
             ts,
             (uint64_t)(esp_log_timestamp() / 1000),
             wifi_status.rssi,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
             vbat_status.vbat_mv,
             wifi_state_name(wifi_status.state));

    mqtt_publish_if_connected(s_topic_heartbeat, payload, CONFIG_MQTT_QOS_TELEMETRY, 0);
}

static void mqtt_presence_publish_event(const char *event_name, const char *message)
{
    char ts[MQTT_TS_BUF_SIZE] = {0};
    char payload[MQTT_JSON_BUF_SIZE] = {0};
    char event_escaped[64] = {0};
    char message_escaped[128] = {0};

    mqtt_presence_build_timestamp(ts, sizeof(ts));
    json_escape_copy(event_escaped, sizeof(event_escaped), event_name ? event_name : "event");
    json_escape_copy(message_escaped, sizeof(message_escaped), message ? message : "");

    snprintf(payload,
             sizeof(payload),
             "{\"device_id\":\"%s\",\"fw\":\"%s\",\"session_id\":\"%s\",\"event\":\"%s\",\"message\":\"%s\",\"ts\":\"%s\"}",
             s_device_id,
             APP_VERSION_STRING,
             s_session_id,
             event_escaped,
             message_escaped,
             ts);

    mqtt_publish_if_connected(s_topic_event, payload, CONFIG_MQTT_QOS_CRITICAL, 0);
}

static void mqtt_presence_heartbeat_task(void *arg)
{
    (void)arg;

    while (true) {
        if (s_restart_requested) {
            uint32_t now_ms = esp_log_timestamp();
            if (now_ms >= s_restart_at_ms) {
                ESP_LOGW(TAG, "Executando reboot agendado via MQTT");
                esp_restart();
            }
        }

        if (!s_client_started) {
            (void)mqtt_presence_start_client_if_needed();
        }

        if (s_connected) {
            mqtt_presence_publish_heartbeat();
        }
        vTaskDelay(pdMS_TO_TICKS(s_heartbeat_interval_s * 1000));
    }
}

static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    (void)handler_args;
    (void)base;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "MQTT conectado | broker=%s", CONFIG_MQTT_BROKER_URI);
        esp_mqtt_client_subscribe(s_client, s_topic_cmd_in, CONFIG_MQTT_QOS_CRITICAL);
        mqtt_presence_publish_status("online", "mqtt_connected");
        mqtt_presence_publish_state_snapshot();
        mqtt_presence_publish_event("boot", "mqtt_online");
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "MQTT desconectado");
        break;

    case MQTT_EVENT_DATA:
        if (event && event->topic && event->data &&
            event->topic_len == (int)strlen(s_topic_cmd_in) &&
            strncmp(event->topic, s_topic_cmd_in, event->topic_len) == 0) {
            char data_buf[192] = {0};
            size_t copy_len = (size_t)event->data_len;
            if (copy_len >= sizeof(data_buf)) {
                copy_len = sizeof(data_buf) - 1;
            }
            memcpy(data_buf, event->data, copy_len);
            data_buf[copy_len] = '\0';
            mqtt_presence_handle_command_payload(data_buf);
        }
        break;

    case MQTT_EVENT_ERROR:
        if (event && event->error_handle) {
            ESP_LOGW(TAG,
                     "MQTT erro | type=%d | connect_rc=%d | tls_esp_err=%s(0x%x) | tls_stack_err=0x%x | cert_flags=0x%x | sock_errno=%d",
                     (int)event->error_handle->error_type,
                     (int)event->error_handle->connect_return_code,
                     esp_err_to_name(event->error_handle->esp_tls_last_esp_err),
                     (unsigned)event->error_handle->esp_tls_last_esp_err,
                     (unsigned)event->error_handle->esp_tls_stack_err,
                     (unsigned)event->error_handle->esp_tls_cert_verify_flags,
                     event->error_handle->esp_transport_sock_errno);
        } else {
            ESP_LOGW(TAG, "MQTT erro de transporte/protocolo (sem detalhes)");
        }
        break;

    default:
        break;
    }
}

static void mqtt_presence_generate_device_id(void)
{
    uint8_t mac[6] = {0};
    char prefix[40] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);

    mqtt_presence_normalize_id_prefix(prefix, sizeof(prefix), CONFIG_ACR_UPLOAD_PREFIX);

    if (err == ESP_OK) {
        snprintf(s_device_id,
                 sizeof(s_device_id),
                 "%s-%02X%02X%02X",
                 prefix,
                 (unsigned)mac[3],
                 (unsigned)mac[4],
                 (unsigned)mac[5]);
    } else {
        snprintf(s_device_id, sizeof(s_device_id), "%s-UNKNOWN", prefix);
    }
}

static void mqtt_presence_build_topics(void)
{
    snprintf(s_topic_status,
             sizeof(s_topic_status),
             "%s/%s/status",
             CONFIG_MQTT_TOPIC_NAMESPACE,
             s_device_id);
    snprintf(s_topic_heartbeat,
             sizeof(s_topic_heartbeat),
             "%s/%s/heartbeat",
             CONFIG_MQTT_TOPIC_NAMESPACE,
             s_device_id);
    snprintf(s_topic_state,
             sizeof(s_topic_state),
             "%s/%s/state",
             CONFIG_MQTT_TOPIC_NAMESPACE,
             s_device_id);
    snprintf(s_topic_event,
             sizeof(s_topic_event),
             "%s/%s/event",
             CONFIG_MQTT_TOPIC_NAMESPACE,
             s_device_id);
    snprintf(s_topic_cmd_in,
             sizeof(s_topic_cmd_in),
             "%s/%s/cmd/in",
             CONFIG_MQTT_TOPIC_NAMESPACE,
             s_device_id);
    snprintf(s_topic_cmd_out,
             sizeof(s_topic_cmd_out),
             "%s/%s/cmd/out",
             CONFIG_MQTT_TOPIC_NAMESPACE,
             s_device_id);
}

static esp_err_t mqtt_presence_start_client_if_needed(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {0};
    wifi_manager_status_t wifi_status = {0};

    if (s_client_started) {
        return ESP_OK;
    }

    if (wifi_manager_get_status(&wifi_status) != ESP_OK ||
        wifi_status.state != WIFI_MANAGER_STATE_STA_CONNECTED ||
        !wifi_status.wifi_ready) {
        ESP_LOGI(TAG, "Aguardando STA conectada para iniciar MQTT");
        return ESP_OK;
    }

    if (!time_sync_is_synchronized()) {
        uint32_t now_ms = esp_log_timestamp();
        if (s_last_wait_log_ms == 0 || (now_ms - s_last_wait_log_ms) >= 10000) {
            ESP_LOGI(TAG, "Aguardando sincronizacao de horario (NTP) para validar TLS MQTT");
            s_last_wait_log_ms = now_ms;
        }
        return ESP_OK;
    }

    mqtt_presence_ensure_session_id();

    snprintf(s_lwt_payload,
             sizeof(s_lwt_payload),
             "{\"device_id\":\"%s\",\"fw\":\"%s\",\"session_id\":\"%s\",\"status\":\"offline\",\"reason\":\"lwt\"}",
             s_device_id,
             APP_VERSION_STRING,
             s_session_id);

    mqtt_cfg.broker.address.hostname = s_broker_host;
    mqtt_cfg.broker.address.port = s_broker_port;
    mqtt_cfg.broker.address.transport = s_broker_transport;
    mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    mqtt_cfg.session.keepalive = CONFIG_MQTT_KEEPALIVE_S;
    mqtt_cfg.session.last_will.topic = s_topic_status;
    mqtt_cfg.session.last_will.msg = s_lwt_payload;
    mqtt_cfg.session.last_will.qos = CONFIG_MQTT_QOS_CRITICAL;
    mqtt_cfg.session.last_will.retain = true;

    if (CONFIG_MQTT_USERNAME[0] != '\0') {
        mqtt_cfg.credentials.username = CONFIG_MQTT_USERNAME;
    }
    if (CONFIG_MQTT_PASSWORD[0] != '\0') {
        mqtt_cfg.credentials.authentication.password = CONFIG_MQTT_PASSWORD;
    }

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Falha ao inicializar cliente MQTT");
        return ESP_FAIL;
    }

    esp_err_t err = esp_mqtt_client_register_event(s_client,
                                                   ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler,
                                                   NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar handler MQTT: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar cliente MQTT: %s", esp_err_to_name(err));
        return err;
    }

    s_client_started = true;
    ESP_LOGI(TAG,
             "Cliente MQTT iniciado | host=%s | port=%" PRIu32 " | session_id=%s",
             s_broker_host,
             s_broker_port,
             s_session_id);
    return ESP_OK;
}

static void mqtt_presence_wifi_event_handler(void *arg,
                                             esp_event_base_t event_base,
                                             int32_t event_id,
                                             void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    if (event_id == WIFI_MANAGER_EVENT_STA_CONNECTED) {
        (void)mqtt_presence_start_client_if_needed();
    }
}

esp_err_t mqtt_presence_start(void)
{
#if !CONFIG_MQTT_PRESENCE_ENABLED
    return ESP_OK;
#else
    if (s_started) {
        return ESP_OK;
    }

    if (CONFIG_MQTT_BROKER_URI[0] == '\0') {
        ESP_LOGW(TAG, "MQTT habilitado sem broker URI; modulo nao iniciado");
        return ESP_ERR_INVALID_ARG;
    }

    mqtt_presence_generate_device_id();
    mqtt_presence_build_topics();
    s_heartbeat_interval_s = CONFIG_MQTT_HEARTBEAT_INTERVAL_S;
    if (!mqtt_presence_parse_broker_uri(CONFIG_MQTT_BROKER_URI,
                                        s_broker_host,
                                        sizeof(s_broker_host),
                                        &s_broker_port,
                                        &s_broker_transport)) {
        ESP_LOGE(TAG,
                 "MQTT_BROKER_URI invalida: %s (esperado mqtt://host:port ou mqtts://host:port)",
                 CONFIG_MQTT_BROKER_URI);
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_wifi_event_registered) {
        esp_err_t err = esp_event_handler_register(WIFI_MANAGER_EVENT,
                                                   ESP_EVENT_ANY_ID,
                                                   mqtt_presence_wifi_event_handler,
                                                   NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao registrar eventos Wi-Fi para MQTT: %s", esp_err_to_name(err));
            return err;
        }
        s_wifi_event_registered = true;
    }

    BaseType_t task_ok = xTaskCreate(mqtt_presence_heartbeat_task,
                                     "mqtt_hb",
                                     4096,
                                     NULL,
                                     tskIDLE_PRIORITY + 1,
                                     &s_heartbeat_task);
    if (task_ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    (void)mqtt_presence_start_client_if_needed();
    ESP_LOGI(TAG,
             "MQTT presence inicializado | device_id=%s | namespace=%s | broker=%s:%" PRIu32,
             s_device_id,
             CONFIG_MQTT_TOPIC_NAMESPACE,
             s_broker_host,
             s_broker_port);
    return ESP_OK;
#endif
}
