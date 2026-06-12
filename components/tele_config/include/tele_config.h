#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef TELE_CONFIG_HOST_TEST
#include "tele_config_host_stubs.h"
#else
#include "cJSON.h"
#include "esp_err.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define TELE_CONFIG_ID_MAX_LEN 48
#define TELE_CONFIG_NVS_KEY_MAX_LEN 15
#define TELE_CONFIG_STRING_MAX_LEN 128

typedef enum {
    TELE_CONFIG_TYPE_BOOL = 0,
    TELE_CONFIG_TYPE_I32,
    TELE_CONFIG_TYPE_U32,
    TELE_CONFIG_TYPE_STRING,
    TELE_CONFIG_TYPE_ENUM,
} tele_config_type_t;

typedef enum {
    TELE_CONFIG_FLAG_WEB = 1U << 0,
    TELE_CONFIG_FLAG_MQTT = 1U << 1,
    TELE_CONFIG_FLAG_SECRET = 1U << 2,
    TELE_CONFIG_FLAG_REBOOT_REQUIRED = 1U << 3,
    TELE_CONFIG_FLAG_READ_ONLY = 1U << 4,
} tele_config_flags_t;

typedef union {
    bool boolean;
    int32_t i32;
    uint32_t u32;
    const char *string;
} tele_config_default_value_t;

typedef union {
    int32_t i32;
    uint32_t u32;
} tele_config_limit_t;

typedef union {
    bool boolean;
    int32_t i32;
    uint32_t u32;
    const char *string;
} tele_config_value_t;

typedef struct {
    const char *id;
    const char *nvs_key;
    tele_config_type_t type;
    tele_config_default_value_t default_value;
    tele_config_limit_t min;
    tele_config_limit_t max;
    size_t min_len;
    size_t max_len;
    uint32_t flags;
} tele_config_field_t;

typedef esp_err_t (*tele_config_apply_cb_t)(const tele_config_field_t *field,
                                            const tele_config_value_t *value,
                                            void *ctx);

esp_err_t tele_config_validate_value(const tele_config_field_t *field,
                                     const tele_config_value_t *value);
esp_err_t tele_config_register_fields(const tele_config_field_t *fields, size_t field_count);
const tele_config_field_t *tele_config_find_field(const char *id);
esp_err_t tele_config_set_apply_handler(const char *id, tele_config_apply_cb_t apply_cb, void *ctx);
esp_err_t tele_config_apply_value(const char *id, const tele_config_value_t *value);
esp_err_t tele_config_get_effective(const char *id,
                                    tele_config_value_t *out_value,
                                    char *string_buffer,
                                    size_t string_buffer_size,
                                    bool *out_from_nvs);
esp_err_t tele_config_set_override(const char *id, const tele_config_value_t *value);
esp_err_t tele_config_reset_override(const char *id);
esp_err_t tele_config_add_manifest_to_json(cJSON *root, uint32_t required_flags);

#ifdef __cplusplus
}
#endif
