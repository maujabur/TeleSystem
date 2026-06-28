#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tele_channels.h"

#ifdef TELE_STATUS_HOST_TEST
#include "tele_status_host_stubs.h"
#else
#include "cJSON.h"
#include "esp_err.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define TELE_STATUS_ID_MAX_LEN 48

typedef enum {
    TELE_STATUS_TYPE_BOOL = 0,
    TELE_STATUS_TYPE_I32,
    TELE_STATUS_TYPE_U32,
    TELE_STATUS_TYPE_STRING,
} tele_status_type_t;

typedef enum {
    TELE_STATUS_FLAG_STATE = 1U << 0,
    TELE_STATUS_FLAG_HEARTBEAT = 1U << 1,
    TELE_STATUS_FLAG_TECHNICAL = 1U << 2,
    TELE_STATUS_FLAG_SENSITIVE = 1U << 3,
} tele_status_flags_t;

typedef bool (*tele_status_bool_reader_t)(void *ctx);
typedef int32_t (*tele_status_i32_reader_t)(void *ctx);
typedef uint32_t (*tele_status_u32_reader_t)(void *ctx);
typedef const char *(*tele_status_string_reader_t)(void *ctx);

typedef union {
    tele_status_bool_reader_t boolean;
    tele_status_i32_reader_t i32;
    tele_status_u32_reader_t u32;
    tele_status_string_reader_t string;
} tele_status_reader_t;

typedef struct {
    const char *id;
    const char *label;
    const char *description;
    const char *group;
    tele_status_type_t type;
    const char *unit;
    uint32_t channel_flags;
    uint32_t flags;
    tele_status_reader_t read;
    void *ctx;
} tele_status_field_t;

esp_err_t tele_status_register_fields(const tele_status_field_t *fields, size_t field_count);
const tele_status_field_t *tele_status_find_field(const char *id);
esp_err_t tele_status_add_fields_to_json(cJSON *root,
                                         uint32_t required_channel_flags,
                                         uint32_t required_flags);
esp_err_t tele_status_add_manifest_to_json(cJSON *root, uint32_t required_channel_flags);

#ifdef __cplusplus
}
#endif
