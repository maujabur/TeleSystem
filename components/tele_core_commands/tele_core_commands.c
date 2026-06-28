#include "tele_core_commands.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tele_commands.h"
#include "tele_config.h"
#include "tele_status.h"

#define TELE_CORE_RESTART_DELAY_DEFAULT_MS 800
#define TELE_CORE_RESTART_DELAY_MIN_MS 100
#define TELE_CORE_RESTART_DELAY_MAX_MS 10000

static tele_core_commands_config_t s_config;

static esp_err_t handle_ping_command(const char *cmd_name,
                                     const cJSON *args,
                                     cJSON **out_result,
                                     const char **out_error,
                                     uint32_t required_flags,
                                     void *ctx);
static esp_err_t handle_get_state_command(const char *cmd_name,
                                          const cJSON *args,
                                          cJSON **out_result,
                                          const char **out_error,
                                          uint32_t required_flags,
                                          void *ctx);
static esp_err_t handle_get_technical_status_command(const char *cmd_name,
                                                     const cJSON *args,
                                                     cJSON **out_result,
                                                     const char **out_error,
                                                     uint32_t required_flags,
                                                     void *ctx);
static esp_err_t handle_config_get_command(const char *cmd_name,
                                           const cJSON *args,
                                           cJSON **out_result,
                                           const char **out_error,
                                           uint32_t required_flags,
                                           void *ctx);
static esp_err_t handle_commands_get_command(const char *cmd_name,
                                             const cJSON *args,
                                             cJSON **out_result,
                                             const char **out_error,
                                             uint32_t required_flags,
                                             void *ctx);
static esp_err_t handle_config_set_command(const char *cmd_name,
                                           const cJSON *args,
                                           cJSON **out_result,
                                           const char **out_error,
                                           uint32_t required_flags,
                                           void *ctx);
static esp_err_t handle_config_reset_command(const char *cmd_name,
                                             const cJSON *args,
                                             cJSON **out_result,
                                             const char **out_error,
                                             uint32_t required_flags,
                                             void *ctx);
static esp_err_t handle_apply_and_reboot_command(const char *cmd_name,
                                                 const cJSON *args,
                                                 cJSON **out_result,
                                                 const char **out_error,
                                                 uint32_t required_flags,
                                                 void *ctx);

static const tele_command_arg_t s_cmd_config_field_args[] = {
    {
        .id = "id",
        .type = TELE_COMMAND_ARG_STRING,
        .required = true,
        .min_len = 1,
        .max_len = TELE_CONFIG_ID_MAX_LEN,
    },
};

static const tele_command_arg_t s_cmd_config_set_args[] = {
    {
        .id = "id",
        .type = TELE_COMMAND_ARG_STRING,
        .required = true,
        .min_len = 1,
        .max_len = TELE_CONFIG_ID_MAX_LEN,
    },
    {
        .id = "value",
        .type = TELE_COMMAND_ARG_ANY,
        .required = true,
    },
};

static const tele_command_arg_t s_cmd_reboot_args[] = {
    {
        .id = "delay_ms",
        .type = TELE_COMMAND_ARG_U32,
        .required = false,
        .min.u32 = TELE_CORE_RESTART_DELAY_MIN_MS,
        .max.u32 = TELE_CORE_RESTART_DELAY_MAX_MS,
    },
};

static const tele_command_t s_builtin_commands[] = {
    {
        .name = "ping",
        .label = "Ping",
        .description = "Solicita uma resposta imediata do equipamento.",
        .group = "system",
        .flags = TELE_COMMAND_FLAG_MQTT | TELE_COMMAND_FLAG_WEB,
        .handler = handle_ping_command,
    },
    {
        .name = "get_state",
        .label = "Ler estado",
        .description = "Solicita um snapshot curto de estado/conectividade.",
        .group = "status",
        .flags = TELE_COMMAND_FLAG_MQTT | TELE_COMMAND_FLAG_WEB,
        .handler = handle_get_state_command,
    },
    {
        .name = "get_technical_status",
        .label = "Status tecnico",
        .description = "Solicita diagnostico detalhado de runtime, energia e sensores.",
        .group = "status",
        .flags = TELE_COMMAND_FLAG_MQTT | TELE_COMMAND_FLAG_WEB,
        .handler = handle_get_technical_status_command,
    },
    {
        .name = "config/get",
        .label = "Ler configuracao",
        .description = "Solicita o manifesto de configuracao atual.",
        .group = "config",
        .flags = TELE_COMMAND_FLAG_MQTT | TELE_COMMAND_FLAG_WEB,
        .handler = handle_config_get_command,
    },
    {
        .name = "commands/get",
        .label = "Ler comandos",
        .description = "Solicita o manifesto de comandos suportados.",
        .group = "system",
        .flags = TELE_COMMAND_FLAG_MQTT | TELE_COMMAND_FLAG_WEB,
        .handler = handle_commands_get_command,
    },
    {
        .name = "config/set",
        .label = "Salvar configuracao",
        .description = "Atualiza um campo configuravel exposto pelo canal.",
        .group = "config",
        .flags = TELE_COMMAND_FLAG_MQTT | TELE_COMMAND_FLAG_WEB | TELE_COMMAND_FLAG_MUTATING,
        .args = s_cmd_config_set_args,
        .arg_count = 2,
        .handler = handle_config_set_command,
    },
    {
        .name = "config/reset",
        .label = "Resetar configuracao",
        .description = "Remove o override de um campo e volta ao default.",
        .group = "config",
        .flags = TELE_COMMAND_FLAG_MQTT | TELE_COMMAND_FLAG_WEB | TELE_COMMAND_FLAG_MUTATING,
        .args = s_cmd_config_field_args,
        .arg_count = 1,
        .handler = handle_config_reset_command,
    },
    {
        .name = "apply_and_reboot",
        .label = "Aplicar e reiniciar",
        .description = "Agenda um reboot curto depois do ACK.",
        .group = "system",
        .flags = TELE_COMMAND_FLAG_MQTT | TELE_COMMAND_FLAG_WEB | TELE_COMMAND_FLAG_MUTATING |
                 TELE_COMMAND_FLAG_REBOOT_REQUIRED,
        .args = s_cmd_reboot_args,
        .arg_count = 1,
        .handler = handle_apply_and_reboot_command,
    },
};

static cJSON *build_status_fields_payload(tele_status_flags_t flags)
{
    cJSON *result = cJSON_CreateObject();
    if (!result) {
        return NULL;
    }

    if (tele_status_add_fields_to_json(result, flags) != ESP_OK) {
        cJSON_Delete(result);
        return NULL;
    }

    return result;
}

static cJSON *build_default_config_manifest(uint32_t config_flags)
{
    cJSON *result = cJSON_CreateObject();
    if (!result) {
        return NULL;
    }

    if (tele_config_add_manifest_to_json(result, config_flags) != ESP_OK) {
        cJSON_Delete(result);
        return NULL;
    }

    return result;
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

static uint32_t config_flags_from_command_flags(uint32_t command_flags)
{
    uint32_t config_flags = 0;

    if ((command_flags & TELE_COMMAND_FLAG_MQTT) != 0) {
        config_flags |= TELE_CONFIG_FLAG_MQTT;
    }
    if ((command_flags & TELE_COMMAND_FLAG_WEB) != 0) {
        config_flags |= TELE_CONFIG_FLAG_WEB;
    }
    return config_flags;
}

static uint32_t bounded_restart_delay_ms(uint32_t delay_ms)
{
    if (delay_ms < TELE_CORE_RESTART_DELAY_MIN_MS) {
        return TELE_CORE_RESTART_DELAY_MIN_MS;
    }
    if (delay_ms > TELE_CORE_RESTART_DELAY_MAX_MS) {
        return TELE_CORE_RESTART_DELAY_MAX_MS;
    }
    return delay_ms;
}

esp_err_t tele_core_commands_register(const tele_core_commands_config_t *config)
{
    if (config) {
        s_config = *config;
    } else {
        memset(&s_config, 0, sizeof(s_config));
    }

    esp_err_t err = tele_commands_register(s_builtin_commands,
                                           sizeof(s_builtin_commands) / sizeof(s_builtin_commands[0]));
    return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
}

static esp_err_t handle_ping_command(const char *cmd_name,
                                     const cJSON *args,
                                     cJSON **out_result,
                                     const char **out_error,
                                     uint32_t required_flags,
                                     void *ctx)
{
    (void)cmd_name;
    (void)args;
    (void)out_error;
    (void)required_flags;
    (void)ctx;

    *out_result = cJSON_CreateObject();
    if (!*out_result) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(*out_result, "pong", true);
    return ESP_OK;
}

static esp_err_t handle_get_state_command(const char *cmd_name,
                                          const cJSON *args,
                                          cJSON **out_result,
                                          const char **out_error,
                                          uint32_t required_flags,
                                          void *ctx)
{
    (void)cmd_name;
    (void)args;
    (void)out_error;
    (void)required_flags;
    (void)ctx;

    *out_result = s_config.build_state ?
                  s_config.build_state(s_config.ctx) :
                  build_status_fields_payload(TELE_STATUS_FLAG_STATE);
    return *out_result ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t handle_get_technical_status_command(const char *cmd_name,
                                                     const cJSON *args,
                                                     cJSON **out_result,
                                                     const char **out_error,
                                                     uint32_t required_flags,
                                                     void *ctx)
{
    (void)cmd_name;
    (void)args;
    (void)out_error;
    (void)required_flags;
    (void)ctx;

    *out_result = s_config.build_technical_status ?
                  s_config.build_technical_status(s_config.ctx) :
                  cJSON_CreateObject();
    return *out_result ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t handle_config_get_command(const char *cmd_name,
                                           const cJSON *args,
                                           cJSON **out_result,
                                           const char **out_error,
                                           uint32_t required_flags,
                                           void *ctx)
{
    (void)cmd_name;
    (void)args;
    (void)out_error;
    (void)ctx;

    if ((required_flags & TELE_COMMAND_FLAG_WEB) != 0) {
        *out_result = build_default_config_manifest(TELE_CONFIG_FLAG_WEB);
        return *out_result ? ESP_OK : ESP_ERR_NO_MEM;
    }

    *out_result = s_config.build_config_manifest ?
                  s_config.build_config_manifest(s_config.ctx) :
                  build_default_config_manifest(TELE_CONFIG_FLAG_MQTT);
    return *out_result ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t handle_commands_get_command(const char *cmd_name,
                                             const cJSON *args,
                                             cJSON **out_result,
                                             const char **out_error,
                                             uint32_t required_flags,
                                             void *ctx)
{
    (void)cmd_name;
    (void)args;
    (void)ctx;

    *out_result = cJSON_CreateObject();
    if (!*out_result) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = tele_commands_add_manifest_to_json(*out_result, required_flags);
    if (err != ESP_OK) {
        cJSON_Delete(*out_result);
        *out_result = NULL;
        if (out_error) {
            *out_error = "commands_unavailable";
        }
        return err;
    }
    return ESP_OK;
}

static esp_err_t handle_config_set_command(const char *cmd_name,
                                           const cJSON *args,
                                           cJSON **out_result,
                                           const char **out_error,
                                           uint32_t required_flags,
                                           void *ctx)
{
    (void)cmd_name;
    (void)ctx;
    uint32_t config_flags = config_flags_from_command_flags(required_flags);
    cJSON *field_id_item = cJSON_IsObject(args) ?
                           cJSON_GetObjectItemCaseSensitive(args, "id") : NULL;
    cJSON *value_item = cJSON_IsObject(args) ?
                        cJSON_GetObjectItemCaseSensitive(args, "value") : NULL;
    const tele_config_field_t *field = NULL;
    tele_config_value_t value = {0};
    tele_config_update_result_t update = {0};

    if (!cJSON_IsObject(args)) {
        *out_error = "missing_args_object";
        return ESP_ERR_INVALID_ARG;
    }
    if (!cJSON_IsString(field_id_item) || !field_id_item->valuestring ||
        field_id_item->valuestring[0] == '\0') {
        *out_error = "missing_config_id";
        return ESP_ERR_INVALID_ARG;
    }

    field = tele_config_find_field(field_id_item->valuestring);
    if (!field || config_flags == 0 || (field->flags & config_flags) != config_flags) {
        *out_error = "config_field_not_found";
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = tele_config_value_from_json(field, value_item, &value, out_error);
    if (err != ESP_OK) {
        return err;
    }

    err = tele_config_update_value(field->id, &value, &update);
    *out_result = build_config_update_result(field->id, &update);
    if (err == ESP_OK) {
        if (s_config.config_changed) {
            s_config.config_changed(s_config.ctx);
        }
        return ESP_OK;
    }

    *out_error = "config_update_failed";
    return err;
}

static esp_err_t handle_config_reset_command(const char *cmd_name,
                                             const cJSON *args,
                                             cJSON **out_result,
                                             const char **out_error,
                                             uint32_t required_flags,
                                             void *ctx)
{
    (void)cmd_name;
    (void)ctx;
    uint32_t config_flags = config_flags_from_command_flags(required_flags);
    cJSON *field_id_item = cJSON_IsObject(args) ?
                           cJSON_GetObjectItemCaseSensitive(args, "id") : NULL;
    const tele_config_field_t *field = NULL;
    tele_config_update_result_t update = {0};

    if (!cJSON_IsObject(args)) {
        *out_error = "missing_args_object";
        return ESP_ERR_INVALID_ARG;
    }
    if (!cJSON_IsString(field_id_item) || !field_id_item->valuestring ||
        field_id_item->valuestring[0] == '\0') {
        *out_error = "missing_config_id";
        return ESP_ERR_INVALID_ARG;
    }

    field = tele_config_find_field(field_id_item->valuestring);
    if (!field || config_flags == 0 || (field->flags & config_flags) != config_flags) {
        *out_error = "config_field_not_found";
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = tele_config_reset_value(field->id, &update);
    *out_result = build_config_update_result(field->id, &update);
    if (err == ESP_OK) {
        if (s_config.config_changed) {
            s_config.config_changed(s_config.ctx);
        }
        return ESP_OK;
    }

    *out_error = "config_reset_failed";
    return err;
}

static esp_err_t handle_apply_and_reboot_command(const char *cmd_name,
                                                 const cJSON *args,
                                                 cJSON **out_result,
                                                 const char **out_error,
                                                 uint32_t required_flags,
                                                 void *ctx)
{
    (void)cmd_name;
    (void)out_error;
    (void)required_flags;
    (void)ctx;

    cJSON *delay_item = cJSON_IsObject(args) ? cJSON_GetObjectItemCaseSensitive(args, "delay_ms") : NULL;
    uint32_t delay_ms = TELE_CORE_RESTART_DELAY_DEFAULT_MS;

    if (cJSON_IsNumber(delay_item) && delay_item->valuedouble > 0) {
        delay_ms = (uint32_t)delay_item->valuedouble;
    }
    delay_ms = bounded_restart_delay_ms(delay_ms);

    if (s_config.restart) {
        s_config.restart(delay_ms, s_config.ctx);
    }

    *out_result = cJSON_CreateObject();
    if (!*out_result) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(*out_result, "restart_scheduled", true);
    cJSON_AddNumberToObject(*out_result, "restart_delay_ms", (double)delay_ms);
    return ESP_OK;
}
