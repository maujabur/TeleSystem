#ifndef TELE_MQTT_H
#define TELE_MQTT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*tele_mqtt_ready_cb_t)(void *ctx);
typedef bool (*tele_mqtt_timestamp_cb_t)(char *buffer, size_t buffer_len, void *ctx);
typedef cJSON *(*tele_mqtt_json_cb_t)(void *ctx);
typedef esp_err_t (*tele_mqtt_settings_handler_t)(const cJSON *args,
                                                  cJSON **out_result,
                                                  const char **out_error,
                                                  void *ctx);
typedef bool (*tele_mqtt_command_mutating_cb_t)(const char *cmd_name,
                                                const cJSON *args,
                                                void *ctx);
typedef esp_err_t (*tele_mqtt_command_handler_t)(const char *cmd_name,
                                                 const cJSON *args,
                                                 cJSON **out_result,
                                                 const char **out_error,
                                                 void *ctx);
typedef void (*tele_mqtt_restart_cb_t)(uint32_t delay_ms, void *ctx);

typedef struct {
    const char *broker_uri;
    const char *username;
    const char *password;
    const char *topic_namespace;
    const char *device_id_prefix;
    const char *firmware_version;
    uint32_t heartbeat_interval_s;
    uint32_t keepalive_s;
    int qos_critical;
    int qos_telemetry;
    tele_mqtt_ready_cb_t is_ready;
    tele_mqtt_timestamp_cb_t build_timestamp;
    tele_mqtt_json_cb_t build_state;
    tele_mqtt_json_cb_t build_settings;
    tele_mqtt_json_cb_t build_status_manifest;
    tele_mqtt_json_cb_t build_technical_status;
    tele_mqtt_json_cb_t build_heartbeat;
    tele_mqtt_settings_handler_t apply_settings;
    tele_mqtt_command_mutating_cb_t is_mutating_command;
    tele_mqtt_command_handler_t handle_command;
    tele_mqtt_restart_cb_t restart;
    void *ctx;
} tele_mqtt_config_t;

esp_err_t tele_mqtt_start(const tele_mqtt_config_t *config);
esp_err_t tele_mqtt_start_client_if_ready(void);
esp_err_t tele_mqtt_publish_event(const char *event_name, const char *message);
uint32_t tele_mqtt_get_heartbeat_interval_s(void);
bool tele_mqtt_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif
