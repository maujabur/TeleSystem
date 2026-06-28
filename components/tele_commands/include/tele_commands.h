#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tele_channels.h"

#ifdef TELE_COMMANDS_HOST_TEST
#include "tele_commands_host_stubs.h"
#else
#include "cJSON.h"
#include "esp_err.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define TELE_COMMAND_NAME_MAX_LEN 48
#define TELE_COMMAND_ARG_ID_MAX_LEN 32

typedef enum {
    TELE_COMMAND_ARG_ANY = 0,
    TELE_COMMAND_ARG_BOOL,
    TELE_COMMAND_ARG_I32,
    TELE_COMMAND_ARG_U32,
    TELE_COMMAND_ARG_STRING,
    TELE_COMMAND_ARG_OBJECT,
} tele_command_arg_type_t;

typedef enum {
    TELE_COMMAND_FLAG_MUTATING = 1U << 0,
    TELE_COMMAND_FLAG_REBOOT_REQUIRED = 1U << 1,
    TELE_COMMAND_FLAG_INTERNAL = 1U << 2,
} tele_command_flags_t;

typedef union {
    int32_t i32;
    uint32_t u32;
} tele_command_arg_limit_t;

typedef struct {
    const char *id;
    tele_command_arg_type_t type;
    bool required;
    tele_command_arg_limit_t min;
    tele_command_arg_limit_t max;
    size_t min_len;
    size_t max_len;
} tele_command_arg_t;

typedef struct {
    const char *cmd_id;
    const char *name;
    const cJSON *args;
    uint32_t required_channel_flags;
} tele_command_request_t;

typedef struct {
    bool ok;
    const char *error;
    cJSON *result;
} tele_command_response_t;

typedef esp_err_t (*tele_command_handler_t)(const char *cmd_name,
                                            const cJSON *args,
                                            cJSON **out_result,
                                            const char **out_error,
                                            uint32_t channel_flags,
                                            void *ctx);

typedef struct {
    const char *name;
    const char *label;
    const char *description;
    const char *group;
    uint32_t channel_flags;
    uint32_t flags;
    const tele_command_arg_t *args;
    size_t arg_count;
    tele_command_handler_t handler;
    void *ctx;
} tele_command_t;

esp_err_t tele_commands_register(const tele_command_t *commands, size_t command_count);
const tele_command_t *tele_commands_find(const char *name);
esp_err_t tele_commands_add_manifest_to_json(cJSON *root, uint32_t required_channel_flags);
esp_err_t tele_commands_execute(const tele_command_request_t *request,
                                tele_command_response_t *response);
void tele_commands_response_cleanup(tele_command_response_t *response);

#ifdef __cplusplus
}
#endif
