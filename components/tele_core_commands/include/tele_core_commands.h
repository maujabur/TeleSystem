#pragma once

#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef cJSON *(*tele_core_commands_json_cb_t)(void *ctx);
typedef void (*tele_core_commands_config_changed_cb_t)(void *ctx);
typedef void (*tele_core_commands_restart_cb_t)(uint32_t delay_ms, void *ctx);

typedef struct {
    tele_core_commands_json_cb_t build_state;
    tele_core_commands_json_cb_t build_config_manifest;
    tele_core_commands_json_cb_t build_technical_status;
    tele_core_commands_config_changed_cb_t config_changed;
    tele_core_commands_restart_cb_t restart;
    void *ctx;
} tele_core_commands_config_t;

esp_err_t tele_core_commands_register(const tele_core_commands_config_t *config);

#ifdef __cplusplus
}
#endif
