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
typedef void (*tele_mqtt_restart_cb_t)(uint32_t delay_ms, void *ctx);

/*
 * Public integration contract for the generic MQTT core.
 *
 * Required:
 * - broker_uri
 *
 * Optional identity/default fields:
 * - base_topic: MQTT prefix used as {base_topic}/{device_id}/...
 * - device_id_prefix: prefix used before the MAC suffix
 * - firmware_version: string published in common payload fields
 * - heartbeat_interval_s, keepalive_s, qos_critical, qos_telemetry
 *
 * Optional callbacks:
 * - is_ready: delays MQTT startup until network/time/product prerequisites hold
 * - build_timestamp: returns an ISO-like timestamp; fallback is Unix epoch
 * - build_technical_status: product-specific technical diagnostics
 * - restart: product-specific reboot hook; fallback is esp_restart()
 *
 * Optional payload overrides:
 * - build_state, build_heartbeat, build_config_manifest, build_status_manifest
 *
 * If payload overrides are NULL, tele_mqtt builds them from tele_status and
 * tele_config registries. New products should prefer registry fields first and
 * only provide overrides for genuinely product-specific payloads.
 */
typedef struct {
    const char *broker_uri;
    const char *username;
    const char *password;
    const char *base_topic;
    const char *device_id_prefix;
    const char *firmware_version;
    uint32_t heartbeat_interval_s;
    uint32_t keepalive_s;
    int qos_critical;
    int qos_telemetry;
    tele_mqtt_ready_cb_t is_ready;
    tele_mqtt_timestamp_cb_t build_timestamp;
    tele_mqtt_json_cb_t build_state;
    tele_mqtt_json_cb_t build_config_manifest;
    tele_mqtt_json_cb_t build_status_manifest;
    tele_mqtt_json_cb_t build_technical_status;
    tele_mqtt_json_cb_t build_heartbeat;
    tele_mqtt_restart_cb_t restart;
    void *ctx;
} tele_mqtt_config_t;

esp_err_t tele_mqtt_start(const tele_mqtt_config_t *config);
esp_err_t tele_mqtt_start_client_if_ready(void);
esp_err_t tele_mqtt_publish_event(const char *event_name, const char *message);
uint32_t tele_mqtt_get_heartbeat_interval_s(void);
esp_err_t tele_mqtt_apply_heartbeat_interval_s(uint32_t interval_s);
bool tele_mqtt_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif
