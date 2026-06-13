#include "tele_mqtt.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "tele_config.h"

#define TELE_MQTT_TOPIC_BUF_SIZE 128
#define TELE_MQTT_DEVICE_ID_SIZE 64
#define TELE_MQTT_TS_BUF_SIZE 40
#define TELE_MQTT_JSON_BUF_SIZE 512
#define TELE_MQTT_CMD_ID_SIZE 64
#define TELE_MQTT_CMD_DEDUP_WINDOW 16
#define TELE_MQTT_CMD_IN_PAYLOAD_BUF_SIZE 1024
#define TELE_MQTT_SESSION_ID_SIZE 48
#define TELE_MQTT_START_RETRY_DELAY_MS 1000

static const char *TAG = "tele-mqtt";

static tele_mqtt_config_t s_config;
static bool s_started;
static bool s_client_started;
static bool s_connected;
static bool s_restart_requested;
static uint32_t s_restart_at_ms;
static uint32_t s_last_wait_log_ms;
static uint32_t s_heartbeat_interval_s;
static esp_mqtt_client_handle_t s_client;
static TaskHandle_t s_heartbeat_task;
static esp_mqtt_transport_t s_broker_transport;
static uint32_t s_broker_port;
static char s_broker_host[96];
static char s_device_id[TELE_MQTT_DEVICE_ID_SIZE];
static char s_session_id[TELE_MQTT_SESSION_ID_SIZE];
static char s_lwt_payload[TELE_MQTT_JSON_BUF_SIZE];
static char s_topic_availability[TELE_MQTT_TOPIC_BUF_SIZE];
static char s_topic_seen[TELE_MQTT_TOPIC_BUF_SIZE];
static char s_topic_heartbeat[TELE_MQTT_TOPIC_BUF_SIZE];
static char s_topic_state[TELE_MQTT_TOPIC_BUF_SIZE];
static char s_topic_event[TELE_MQTT_TOPIC_BUF_SIZE];
static char s_topic_meta_config[TELE_MQTT_TOPIC_BUF_SIZE];
static char s_topic_meta_status[TELE_MQTT_TOPIC_BUF_SIZE];
static char s_topic_cmd_in[TELE_MQTT_TOPIC_BUF_SIZE];
static char s_topic_cmd_out[TELE_MQTT_TOPIC_BUF_SIZE];
static char s_cmd_in_payload_buf[TELE_MQTT_CMD_IN_PAYLOAD_BUF_SIZE];
static char s_recent_mutating_cmd_ids[TELE_MQTT_CMD_DEDUP_WINDOW][TELE_MQTT_CMD_ID_SIZE];
static size_t s_recent_mutating_cmd_index;

static void publish_config_manifest(void);

static const char *str_or_empty(const char *value)
{
    return value ? value : "";
}

static const char *firmware_version(void)
{
    return s_config.firmware_version && s_config.firmware_version[0] != '\0' ?
           s_config.firmware_version : "unknown";
}

static const char *topic_namespace(void)
{
    return s_config.topic_namespace && s_config.topic_namespace[0] != '\0' ?
           s_config.topic_namespace : "v1/device";
}

static int qos_critical(void)
{
    return s_config.qos_critical >= 0 ? s_config.qos_critical : 1;
}

static int qos_telemetry(void)
{
    return s_config.qos_telemetry >= 0 ? s_config.qos_telemetry : 0;
}

static uint32_t keepalive_s(void)
{
    return s_config.keepalive_s > 0 ? s_config.keepalive_s : 60;
}

static bool is_ready(void)
{
    return !s_config.is_ready || s_config.is_ready(s_config.ctx);
}

static void normalize_id_prefix(char *dst, size_t dst_len, const char *src)
{
    size_t di = 0;

    if (!dst || dst_len == 0) {
        return;
    }

    dst[0] = '\0';

    if (!src || src[0] == '\0') {
        snprintf(dst, dst_len, "device");
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
        snprintf(dst, dst_len, "device");
        return;
    }

    dst[di] = '\0';
}

static bool parse_broker_uri(const char *uri,
                             char *host,
                             size_t host_len,
                             uint32_t *port,
                             esp_mqtt_transport_t *transport)
{
    const char *host_start = NULL;
    const char *host_end = NULL;
    size_t host_size = 0;
    uint32_t parsed_port = 0;

    if (!uri || !host || host_len == 0 || !port || !transport) {
        return false;
    }

    if (strncmp(uri, "mqtts://", 8) == 0) {
        host_start = uri + 8;
        *transport = MQTT_TRANSPORT_OVER_SSL;
        parsed_port = 8883;
    } else if (strncmp(uri, "mqtt://", 7) == 0) {
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
    return true;
}

static void build_timestamp(char *buffer, size_t buffer_len)
{
    if (!buffer || buffer_len == 0) {
        return;
    }

    if (!s_config.build_timestamp ||
        !s_config.build_timestamp(buffer, buffer_len, s_config.ctx)) {
        snprintf(buffer, buffer_len, "1970-01-01T00:00:00Z");
    }
}

static void ensure_session_id(void)
{
    uint8_t mac[6] = {0};
    esp_err_t mac_err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    time_t now = 0;
    struct tm now_tm = {0};

    if (s_session_id[0] != '\0') {
        return;
    }

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

    snprintf(s_session_id,
             sizeof(s_session_id),
             "boot-%08" PRIX32 "-%02X%02X%02X",
             esp_log_timestamp(),
             mac_err == ESP_OK ? (unsigned)mac[3] : 0u,
             mac_err == ESP_OK ? (unsigned)mac[4] : 0u,
             mac_err == ESP_OK ? (unsigned)mac[5] : 0u);
}

static void generate_device_id(void)
{
    uint8_t mac[6] = {0};
    char prefix[40] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);

    normalize_id_prefix(prefix, sizeof(prefix), s_config.device_id_prefix);

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

    ESP_LOGI(TAG, "Device ID MQTT: %s (prefixo=%s)", s_device_id, prefix);
}

static void build_topics(void)
{
    snprintf(s_topic_availability,
             sizeof(s_topic_availability),
             "%s/%s/availability",
             topic_namespace(),
             s_device_id);
    snprintf(s_topic_seen, sizeof(s_topic_seen), "%s/%s/seen", topic_namespace(), s_device_id);
    snprintf(s_topic_heartbeat, sizeof(s_topic_heartbeat), "%s/%s/heartbeat", topic_namespace(), s_device_id);
    snprintf(s_topic_state, sizeof(s_topic_state), "%s/%s/state", topic_namespace(), s_device_id);
    snprintf(s_topic_event, sizeof(s_topic_event), "%s/%s/event", topic_namespace(), s_device_id);
    snprintf(s_topic_meta_config, sizeof(s_topic_meta_config), "%s/%s/meta/config", topic_namespace(), s_device_id);
    snprintf(s_topic_meta_status, sizeof(s_topic_meta_status), "%s/%s/meta/status", topic_namespace(), s_device_id);
    snprintf(s_topic_cmd_in, sizeof(s_topic_cmd_in), "%s/%s/cmd/in", topic_namespace(), s_device_id);
    snprintf(s_topic_cmd_out, sizeof(s_topic_cmd_out), "%s/%s/cmd/out", topic_namespace(), s_device_id);
}

static int publish_if_connected(const char *topic, const char *payload, int qos, int retain)
{
    if (!s_connected || !s_client || !topic || !payload) {
        return -1;
    }

    return esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain);
}

static void add_common_fields(cJSON *root, const char *ts)
{
    if (!root) {
        return;
    }

    cJSON_AddStringToObject(root, "device_id", s_device_id);
    cJSON_AddStringToObject(root, "fw", firmware_version());
    cJSON_AddStringToObject(root, "session_id", s_session_id[0] != '\0' ? s_session_id : "unknown");
    if (ts) {
        cJSON_AddStringToObject(root, "ts", ts);
    }
}

static int publish_json(const char *topic, cJSON *payload, int qos, int retain)
{
    char *payload_text = NULL;
    int msg_id = -1;

    if (!payload) {
        return -1;
    }

    payload_text = cJSON_PrintUnformatted(payload);
    if (payload_text) {
        msg_id = publish_if_connected(topic, payload_text, qos, retain);
        if (msg_id < 0) {
            ESP_LOGW(TAG,
                     "Falha ao publicar MQTT | topic=%s | qos=%d | retain=%d",
                     topic ? topic : "(null)",
                     qos,
                     retain);
        }
        cJSON_free(payload_text);
    }
    return msg_id;
}

static int publish_json_with_common(const char *topic, cJSON *payload, int qos, int retain)
{
    char ts[TELE_MQTT_TS_BUF_SIZE] = {0};

    if (!payload) {
        return -1;
    }

    build_timestamp(ts, sizeof(ts));
    add_common_fields(payload, ts);
    return publish_json(topic, payload, qos, retain);
}

static bool cmd_id_seen(const char *cmd_id)
{
    if (!cmd_id || cmd_id[0] == '\0') {
        return false;
    }

    for (size_t index = 0; index < TELE_MQTT_CMD_DEDUP_WINDOW; ++index) {
        if (strncmp(s_recent_mutating_cmd_ids[index], cmd_id, TELE_MQTT_CMD_ID_SIZE) == 0) {
            return true;
        }
    }

    return false;
}

static void cmd_id_remember(const char *cmd_id)
{
    if (!cmd_id || cmd_id[0] == '\0') {
        return;
    }

    snprintf(s_recent_mutating_cmd_ids[s_recent_mutating_cmd_index],
             TELE_MQTT_CMD_ID_SIZE,
             "%s",
             cmd_id);
    s_recent_mutating_cmd_index = (s_recent_mutating_cmd_index + 1) % TELE_MQTT_CMD_DEDUP_WINDOW;
}

static void publish_command_reply(const char *cmd_id, bool ok, const char *error, const cJSON *result)
{
    char ts[TELE_MQTT_TS_BUF_SIZE] = {0};
    cJSON *root = cJSON_CreateObject();

    if (!root) {
        return;
    }

    build_timestamp(ts, sizeof(ts));
    add_common_fields(root, ts);
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

    publish_json(s_topic_cmd_out, root, qos_critical(), 0);
    cJSON_Delete(root);
}

static bool reject_duplicate_mutating_command(const char *cmd_id)
{
    cJSON *result = NULL;

    if (!cmd_id_seen(cmd_id)) {
        return false;
    }

    result = cJSON_CreateObject();
    if (result) {
        cJSON_AddBoolToObject(result, "duplicate", true);
        cJSON_AddBoolToObject(result, "executed", false);
    }
    publish_command_reply(cmd_id, true, NULL, result);
    cJSON_Delete(result);
    ESP_LOGW(TAG, "Comando mutavel duplicado ignorado | cmd_id=%s", cmd_id);
    return true;
}

static void schedule_restart(uint32_t delay_ms)
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

static cJSON *build_ping_result(void)
{
    cJSON *result = cJSON_CreateObject();
    if (!result) {
        return NULL;
    }

    cJSON_AddBoolToObject(result, "pong", true);
    cJSON_AddNumberToObject(result, "uptime_s", (double)(esp_log_timestamp() / 1000));
    return result;
}

static esp_err_t json_to_config_value(const tele_config_field_t *field,
                                      const cJSON *json_value,
                                      tele_config_value_t *out_value,
                                      const char **out_error)
{
    if (!field || !json_value || !out_value) {
        if (out_error) {
            *out_error = "missing_config_value";
        }
        return ESP_ERR_INVALID_ARG;
    }

    switch (field->type) {
    case TELE_CONFIG_TYPE_BOOL:
        if (!cJSON_IsBool(json_value)) {
            if (out_error) {
                *out_error = "config_value_type_bool_required";
            }
            return ESP_ERR_INVALID_ARG;
        }
        out_value->boolean = cJSON_IsTrue(json_value);
        return ESP_OK;
    case TELE_CONFIG_TYPE_I32:
    case TELE_CONFIG_TYPE_ENUM:
        if (!cJSON_IsNumber(json_value)) {
            if (out_error) {
                *out_error = "config_value_type_i32_required";
            }
            return ESP_ERR_INVALID_ARG;
        }
        out_value->i32 = (int32_t)json_value->valuedouble;
        return ESP_OK;
    case TELE_CONFIG_TYPE_U32:
        if (!cJSON_IsNumber(json_value) || json_value->valuedouble < 0) {
            if (out_error) {
                *out_error = "config_value_type_u32_required";
            }
            return ESP_ERR_INVALID_ARG;
        }
        out_value->u32 = (uint32_t)json_value->valuedouble;
        return ESP_OK;
    case TELE_CONFIG_TYPE_STRING:
        if (!cJSON_IsString(json_value) || !json_value->valuestring) {
            if (out_error) {
                *out_error = "config_value_type_string_required";
            }
            return ESP_ERR_INVALID_ARG;
        }
        out_value->string = json_value->valuestring;
        return ESP_OK;
    default:
        if (out_error) {
            *out_error = "config_field_type_unsupported";
        }
        return ESP_ERR_INVALID_ARG;
    }
}

static cJSON *build_config_update_result(const char *id, const tele_config_update_result_t *update)
{
    cJSON *result = cJSON_CreateObject();
    if (!result) {
        return NULL;
    }

    cJSON_AddStringToObject(result, "id", id ? id : "");
    if (update) {
        cJSON_AddBoolToObject(result, "stored", update->stored);
        cJSON_AddBoolToObject(result, "applied", update->applied);
        cJSON_AddBoolToObject(result, "requires_reboot", update->requires_reboot);
    }
    return result;
}

static void handle_command_payload(const char *payload)
{
    cJSON *root = NULL;
    cJSON *cmd_id_item = NULL;
    cJSON *name_item = NULL;
    cJSON *args = NULL;
    cJSON *result = NULL;
    const char *cmd_id = "unknown";
    const char *cmd_name = NULL;
    const char *error = NULL;
    bool mutating = false;

    if (!payload) {
        return;
    }

    root = cJSON_Parse(payload);
    if (!root) {
        publish_command_reply("unknown", false, "invalid_json", NULL);
        return;
    }

    cmd_id_item = cJSON_GetObjectItemCaseSensitive(root, "cmd_id");
    if (cJSON_IsString(cmd_id_item) && cmd_id_item->valuestring) {
        cmd_id = cmd_id_item->valuestring;
    }

    name_item = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (!cJSON_IsString(name_item) || !name_item->valuestring) {
        cJSON_Delete(root);
        publish_command_reply(cmd_id, false, "missing_name", NULL);
        return;
    }
    cmd_name = name_item->valuestring;
    args = cJSON_GetObjectItemCaseSensitive(root, "args");

    if (strcmp(cmd_name, "ping") == 0) {
        result = build_ping_result();
        publish_command_reply(cmd_id, true, NULL, result);
        cJSON_Delete(result);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(cmd_name, "get_state") == 0) {
        result = s_config.build_state ? s_config.build_state(s_config.ctx) : cJSON_CreateObject();
        publish_command_reply(cmd_id, result != NULL, result ? NULL : "state_unavailable", result);
        cJSON_Delete(result);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(cmd_name, "config/get") == 0) {
        result = s_config.build_config_manifest ? s_config.build_config_manifest(s_config.ctx) : cJSON_CreateObject();
        publish_command_reply(cmd_id, result != NULL, result ? NULL : "config_unavailable", result);
        cJSON_Delete(result);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(cmd_name, "get_technical_status") == 0) {
        result = s_config.build_technical_status ?
                 s_config.build_technical_status(s_config.ctx) : cJSON_CreateObject();
        publish_command_reply(cmd_id, result != NULL, result ? NULL : "technical_status_unavailable", result);
        cJSON_Delete(result);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(cmd_name, "set_heartbeat_interval") == 0) {
        cJSON *interval = cJSON_IsObject(args) ?
                          cJSON_GetObjectItemCaseSensitive(args, "heartbeat_interval_s") : NULL;

        if (reject_duplicate_mutating_command(cmd_id)) {
            cJSON_Delete(root);
            return;
        }

        if (!cJSON_IsNumber(interval)) {
            cJSON_Delete(root);
            publish_command_reply(cmd_id, false, "missing_heartbeat_interval_s", NULL);
            return;
        }

        if (interval->valuedouble < 15 || interval->valuedouble > 3600) {
            cJSON_Delete(root);
            publish_command_reply(cmd_id, false, "heartbeat_interval_out_of_range", NULL);
            return;
        }

        s_heartbeat_interval_s = (uint32_t)interval->valuedouble;
        cmd_id_remember(cmd_id);

        result = cJSON_CreateObject();
        if (result) {
            cJSON_AddNumberToObject(result, "heartbeat_interval_s", (double)s_heartbeat_interval_s);
        }
        publish_command_reply(cmd_id, true, NULL, result);
        cJSON_Delete(result);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(cmd_name, "config/set") == 0) {
        cJSON *field_id_item = cJSON_IsObject(args) ?
                               cJSON_GetObjectItemCaseSensitive(args, "id") : NULL;
        cJSON *value_item = cJSON_IsObject(args) ?
                            cJSON_GetObjectItemCaseSensitive(args, "value") : NULL;
        const tele_config_field_t *field = NULL;
        tele_config_value_t value = {0};
        tele_config_update_result_t update = {0};
        esp_err_t err = ESP_OK;

        if (reject_duplicate_mutating_command(cmd_id)) {
            cJSON_Delete(root);
            return;
        }

        if (!cJSON_IsObject(args)) {
            cJSON_Delete(root);
            publish_command_reply(cmd_id, false, "missing_args_object", NULL);
            return;
        }

        if (!cJSON_IsString(field_id_item) || !field_id_item->valuestring || field_id_item->valuestring[0] == '\0') {
            cJSON_Delete(root);
            publish_command_reply(cmd_id, false, "missing_config_id", NULL);
            return;
        }

        field = tele_config_find_field(field_id_item->valuestring);
        if (!field || (field->flags & TELE_CONFIG_FLAG_MQTT) == 0) {
            cJSON_Delete(root);
            publish_command_reply(cmd_id, false, "config_field_not_found", NULL);
            return;
        }

        err = json_to_config_value(field, value_item, &value, &error);
        if (err != ESP_OK) {
            cJSON_Delete(root);
            publish_command_reply(cmd_id, false, error ? error : "config_value_invalid", NULL);
            return;
        }

        err = tele_config_update_value(field->id, &value, &update);
        result = build_config_update_result(field->id, &update);
        if (err == ESP_OK) {
            cmd_id_remember(cmd_id);
            publish_command_reply(cmd_id, true, NULL, result);
            publish_config_manifest();
        } else {
            publish_command_reply(cmd_id, false, "config_update_failed", result);
        }
        cJSON_Delete(result);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(cmd_name, "config/reset") == 0) {
        cJSON *field_id_item = cJSON_IsObject(args) ?
                               cJSON_GetObjectItemCaseSensitive(args, "id") : NULL;
        const tele_config_field_t *field = NULL;
        tele_config_update_result_t update = {0};
        esp_err_t err = ESP_OK;

        if (reject_duplicate_mutating_command(cmd_id)) {
            cJSON_Delete(root);
            return;
        }

        if (!cJSON_IsObject(args)) {
            cJSON_Delete(root);
            publish_command_reply(cmd_id, false, "missing_args_object", NULL);
            return;
        }

        if (!cJSON_IsString(field_id_item) || !field_id_item->valuestring || field_id_item->valuestring[0] == '\0') {
            cJSON_Delete(root);
            publish_command_reply(cmd_id, false, "missing_config_id", NULL);
            return;
        }

        field = tele_config_find_field(field_id_item->valuestring);
        if (!field || (field->flags & TELE_CONFIG_FLAG_MQTT) == 0) {
            cJSON_Delete(root);
            publish_command_reply(cmd_id, false, "config_field_not_found", NULL);
            return;
        }

        err = tele_config_reset_value(field->id, &update);
        result = build_config_update_result(field->id, &update);
        if (err == ESP_OK) {
            cmd_id_remember(cmd_id);
            publish_command_reply(cmd_id, true, NULL, result);
            publish_config_manifest();
        } else {
            publish_command_reply(cmd_id, false, "config_reset_failed", result);
        }
        cJSON_Delete(result);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(cmd_name, "apply_and_reboot") == 0) {
        cJSON *delay_item = cJSON_IsObject(args) ? cJSON_GetObjectItemCaseSensitive(args, "delay_ms") : NULL;
        uint32_t delay_ms = 800;

        if (reject_duplicate_mutating_command(cmd_id)) {
            cJSON_Delete(root);
            return;
        }

        if (cJSON_IsNumber(delay_item) && delay_item->valuedouble > 0) {
            delay_ms = (uint32_t)delay_item->valuedouble;
        }

        schedule_restart(delay_ms);
        cmd_id_remember(cmd_id);

        result = cJSON_CreateObject();
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
        publish_command_reply(cmd_id, true, NULL, result);
        cJSON_Delete(result);
        cJSON_Delete(root);
        return;
    }

    if (s_config.is_mutating_command) {
        mutating = s_config.is_mutating_command(cmd_name, args, s_config.ctx);
    }

    if (mutating && reject_duplicate_mutating_command(cmd_id)) {
        cJSON_Delete(root);
        return;
    }

    if (s_config.handle_command) {
        esp_err_t err = s_config.handle_command(cmd_name, args, &result, &error, s_config.ctx);
        if (err == ESP_OK) {
            if (mutating) {
                cmd_id_remember(cmd_id);
            }
            publish_command_reply(cmd_id, true, NULL, result);
            cJSON_Delete(result);
            cJSON_Delete(root);
            return;
        }
        if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_NOT_SUPPORTED) {
            publish_command_reply(cmd_id, false, error ? error : "command_failed", result);
            cJSON_Delete(result);
            cJSON_Delete(root);
            return;
        }
    }

    cJSON_Delete(root);
    publish_command_reply(cmd_id, false, "unsupported_command", NULL);
}

static void publish_availability(const char *status_text, const char *reason)
{
    cJSON *payload = cJSON_CreateObject();
    if (!payload) {
        return;
    }

    cJSON_AddStringToObject(payload, "status", status_text ? status_text : "unknown");
    cJSON_AddStringToObject(payload, "reason", reason ? reason : "runtime");
    publish_json_with_common(s_topic_availability, payload, qos_critical(), 1);
    cJSON_Delete(payload);
}

static void publish_state_snapshot(void)
{
    cJSON *payload = s_config.build_state ? s_config.build_state(s_config.ctx) : NULL;
    if (!payload) {
        return;
    }

    publish_json_with_common(s_topic_state, payload, qos_critical(), 1);
    cJSON_Delete(payload);
}

static void publish_status_manifest(void)
{
    cJSON *payload = s_config.build_status_manifest ? s_config.build_status_manifest(s_config.ctx) : NULL;
    if (!payload) {
        return;
    }

    publish_json_with_common(s_topic_meta_status, payload, qos_critical(), 1);
    cJSON_Delete(payload);
}

static void publish_config_manifest(void)
{
    cJSON *payload = s_config.build_config_manifest ? s_config.build_config_manifest(s_config.ctx) : NULL;
    if (!payload) {
        return;
    }

    publish_json_with_common(s_topic_meta_config, payload, qos_critical(), 1);
    cJSON_Delete(payload);
}

static void publish_seen(const char *reason)
{
    char ts[TELE_MQTT_TS_BUF_SIZE] = {0};
    cJSON *payload = cJSON_CreateObject();
    if (!payload) {
        return;
    }

    build_timestamp(ts, sizeof(ts));
    add_common_fields(payload, ts);
    cJSON_AddStringToObject(payload, "last_seen_ts", ts);
    cJSON_AddStringToObject(payload, "reason", reason ? reason : "runtime");
    publish_json(s_topic_seen, payload, qos_critical(), 1);
    cJSON_Delete(payload);
}

static void publish_heartbeat(void)
{
    cJSON *payload = s_config.build_heartbeat ? s_config.build_heartbeat(s_config.ctx) : cJSON_CreateObject();
    if (!payload) {
        return;
    }

    publish_json_with_common(s_topic_heartbeat, payload, qos_telemetry(), 0);
    cJSON_Delete(payload);
    publish_seen("heartbeat");
}

esp_err_t tele_mqtt_publish_event(const char *event_name, const char *message)
{
    cJSON *payload = cJSON_CreateObject();
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(payload, "event", event_name ? event_name : "event");
    cJSON_AddStringToObject(payload, "message", message ? message : "");
    publish_json_with_common(s_topic_event, payload, qos_critical(), 0);
    cJSON_Delete(payload);
    return ESP_OK;
}

static void heartbeat_task(void *arg)
{
    (void)arg;

    while (true) {
        if (s_restart_requested) {
            uint32_t now_ms = esp_log_timestamp();
            if (now_ms >= s_restart_at_ms) {
                ESP_LOGW(TAG, "Executando reboot agendado via MQTT");
                if (s_config.restart) {
                    s_config.restart(0, s_config.ctx);
                } else {
                    esp_restart();
                }
            }
        }

        if (!s_client_started) {
            (void)tele_mqtt_start_client_if_ready();
            vTaskDelay(pdMS_TO_TICKS(TELE_MQTT_START_RETRY_DELAY_MS));
            continue;
        }

        if (s_connected) {
            publish_heartbeat();
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
        ESP_LOGI(TAG, "MQTT conectado | broker=%s", str_or_empty(s_config.broker_uri));
        esp_mqtt_client_subscribe(s_client, s_topic_cmd_in, qos_critical());
        publish_availability("online", "mqtt_connected");
        publish_config_manifest();
        publish_status_manifest();
        publish_state_snapshot();
        publish_heartbeat();
        (void)tele_mqtt_publish_event("boot", "mqtt_online");
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "MQTT desconectado");
        break;

    case MQTT_EVENT_DATA:
        if (event && event->topic && event->data &&
            event->topic_len == (int)strlen(s_topic_cmd_in) &&
            strncmp(event->topic, s_topic_cmd_in, event->topic_len) == 0) {
            int total_len = event->total_data_len > 0 ? event->total_data_len : event->data_len;
            int offset = event->current_data_offset;

            if (total_len <= 0 || offset < 0 || event->data_len < 0) {
                break;
            }

            if ((size_t)total_len >= sizeof(s_cmd_in_payload_buf)) {
                if (offset == 0) {
                    publish_command_reply("unknown", false, "payload_too_large", NULL);
                }
                break;
            }

            if (offset == 0) {
                memset(s_cmd_in_payload_buf, 0, sizeof(s_cmd_in_payload_buf));
            }

            if ((size_t)(offset + event->data_len) >= sizeof(s_cmd_in_payload_buf)) {
                publish_command_reply("unknown", false, "payload_too_large", NULL);
                break;
            }

            memcpy(s_cmd_in_payload_buf + offset, event->data, (size_t)event->data_len);
            if (offset + event->data_len >= total_len) {
                s_cmd_in_payload_buf[total_len] = '\0';
                handle_command_payload(s_cmd_in_payload_buf);
            }
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

esp_err_t tele_mqtt_start_client_if_ready(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {0};

    if (s_client_started) {
        return ESP_OK;
    }

    if (!is_ready()) {
        uint32_t now_ms = esp_log_timestamp();
        if (s_last_wait_log_ms == 0 || (now_ms - s_last_wait_log_ms) >= 10000) {
            ESP_LOGI(TAG, "Aguardando prerequisitos para iniciar MQTT");
            s_last_wait_log_ms = now_ms;
        }
        return ESP_OK;
    }

    ensure_session_id();

    snprintf(s_lwt_payload,
             sizeof(s_lwt_payload),
             "{\"device_id\":\"%s\",\"fw\":\"%s\",\"session_id\":\"%s\",\"status\":\"offline\",\"reason\":\"lwt\"}",
             s_device_id,
             firmware_version(),
             s_session_id);

    mqtt_cfg.broker.address.hostname = s_broker_host;
    mqtt_cfg.broker.address.port = s_broker_port;
    mqtt_cfg.broker.address.transport = s_broker_transport;
    mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    mqtt_cfg.session.keepalive = keepalive_s();
    mqtt_cfg.session.last_will.topic = s_topic_availability;
    mqtt_cfg.session.last_will.msg = s_lwt_payload;
    mqtt_cfg.session.last_will.qos = qos_critical();
    mqtt_cfg.session.last_will.retain = true;

    if (s_config.username && s_config.username[0] != '\0') {
        mqtt_cfg.credentials.username = s_config.username;
    }
    if (s_config.password && s_config.password[0] != '\0') {
        mqtt_cfg.credentials.authentication.password = s_config.password;
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
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar cliente MQTT: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
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

esp_err_t tele_mqtt_start(const tele_mqtt_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started) {
        return ESP_OK;
    }
    if (!config->broker_uri || config->broker_uri[0] == '\0') {
        ESP_LOGW(TAG, "MQTT habilitado sem broker URI; modulo nao iniciado");
        return ESP_ERR_INVALID_ARG;
    }

    s_config = *config;
    s_heartbeat_interval_s = config->heartbeat_interval_s > 0 ? config->heartbeat_interval_s : 60;
    generate_device_id();
    build_topics();

    if (!parse_broker_uri(config->broker_uri,
                          s_broker_host,
                          sizeof(s_broker_host),
                          &s_broker_port,
                          &s_broker_transport)) {
        ESP_LOGE(TAG,
                 "broker_uri invalida: %s (esperado mqtt://host:port ou mqtts://host:port)",
                 config->broker_uri);
        return ESP_ERR_INVALID_ARG;
    }

    BaseType_t task_ok = xTaskCreate(heartbeat_task,
                                     "tele_mqtt",
                                     4096,
                                     NULL,
                                     tskIDLE_PRIORITY + 1,
                                     &s_heartbeat_task);
    if (task_ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    (void)tele_mqtt_start_client_if_ready();
    ESP_LOGI(TAG,
             "MQTT inicializado | device_id=%s | namespace=%s | broker=%s:%" PRIu32,
             s_device_id,
             topic_namespace(),
             s_broker_host,
             s_broker_port);
    return ESP_OK;
}

uint32_t tele_mqtt_get_heartbeat_interval_s(void)
{
    return s_heartbeat_interval_s;
}

bool tele_mqtt_is_connected(void)
{
    return s_connected;
}
