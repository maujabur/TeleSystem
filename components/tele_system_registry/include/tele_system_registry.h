#ifndef TELE_SYSTEM_REGISTRY_H
#define TELE_SYSTEM_REGISTRY_H

#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TELE_SYSTEM_REGISTRY_CONFIG_ID_HEARTBEAT_INTERVAL "mqtt.heartbeat_interval_s"

typedef uint32_t (*tele_system_registry_u32_cb_t)(void *ctx);
typedef esp_err_t (*tele_system_registry_u32_apply_cb_t)(uint32_t value, void *ctx);

typedef struct {
    uint32_t heartbeat_interval_default_s;
    tele_system_registry_u32_cb_t get_heartbeat_interval_s;
    tele_system_registry_u32_apply_cb_t apply_heartbeat_interval_s;
    void *ctx;
} tele_system_registry_config_t;

esp_err_t tele_system_registry_register(const tele_system_registry_config_t *config);
uint32_t tele_system_registry_get_effective_heartbeat_interval_s(uint32_t fallback_s);
cJSON *tele_system_registry_build_technical_status(void);

#ifdef __cplusplus
}
#endif

#endif
