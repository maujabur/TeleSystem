#include "tele_artifacts.h"

#include <string.h>

#ifndef TELE_ARTIFACTS_HOST_TEST
#include "cJSON.h"
#include "tele_commands.h"
#endif

#define TELE_ARTIFACTS_MAX_HANDLERS 8

static const tele_artifact_handler_t *s_handlers[TELE_ARTIFACTS_MAX_HANDLERS];
static size_t s_handler_count;

static bool text_valid(const char *text)
{
    return text && text[0] != '\0';
}

static const char *mode_name(tele_artifact_mode_t mode)
{
    switch (mode) {
    case TELE_ARTIFACT_MODE_FILE:
        return "file";
    case TELE_ARTIFACT_MODE_STREAM:
        return "stream";
    default:
        return "unknown";
    }
}

esp_err_t tele_artifacts_register(const tele_artifact_handler_t *handler)
{
    if (!handler ||
        !text_valid(handler->artifact_type) ||
        !handler->check ||
        !handler->apply) {
        return ESP_ERR_INVALID_ARG;
    }

    if (tele_artifacts_find(handler->artifact_type)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_handler_count >= TELE_ARTIFACTS_MAX_HANDLERS) {
        return ESP_ERR_NO_MEM;
    }

    s_handlers[s_handler_count++] = handler;
    return ESP_OK;
}

const tele_artifact_handler_t *tele_artifacts_find(const char *artifact_type)
{
    if (!text_valid(artifact_type)) {
        return NULL;
    }

    for (size_t i = 0; i < s_handler_count; ++i) {
        if (strcmp(s_handlers[i]->artifact_type, artifact_type) == 0) {
            return s_handlers[i];
        }
    }

    return NULL;
}

esp_err_t tele_artifacts_check(const tele_artifact_request_t *request,
                               tele_artifact_check_result_t *out_result)
{
    if (!request ||
        !text_valid(request->artifact_type) ||
        !text_valid(request->manifest_url) ||
        !out_result) {
        return ESP_ERR_INVALID_ARG;
    }

    const tele_artifact_handler_t *handler = tele_artifacts_find(request->artifact_type);
    if (!handler) {
        return ESP_ERR_NOT_FOUND;
    }

    memset(out_result, 0, sizeof(*out_result));
    return handler->check(request, out_result, handler->ctx);
}

esp_err_t tele_artifacts_apply(const tele_artifact_request_t *request,
                               tele_artifact_apply_result_t *out_result)
{
    if (!request ||
        !text_valid(request->artifact_type) ||
        !text_valid(request->manifest_url) ||
        !out_result) {
        return ESP_ERR_INVALID_ARG;
    }

    const tele_artifact_handler_t *handler = tele_artifacts_find(request->artifact_type);
    if (!handler) {
        return ESP_ERR_NOT_FOUND;
    }

    memset(out_result, 0, sizeof(*out_result));
    return handler->apply(request, out_result, handler->ctx);
}

#ifndef TELE_ARTIFACTS_HOST_TEST

static const tele_command_arg_t artifact_command_args[] = {
    {
        .id = "artifact_type",
        .type = TELE_COMMAND_ARG_STRING,
        .required = true,
        .min_len = 1,
        .max_len = TELE_ARTIFACT_TYPE_SIZE - 1,
    },
    {
        .id = "manifest_url",
        .type = TELE_COMMAND_ARG_STRING,
        .required = true,
        .min_len = 1,
        .max_len = TELE_MANIFEST_URL_SIZE - 1,
    },
    {
        .id = "channel",
        .type = TELE_COMMAND_ARG_STRING,
        .required = false,
        .max_len = TELE_MANIFEST_CHANNEL_SIZE - 1,
    },
    {
        .id = "allow_same_version",
        .type = TELE_COMMAND_ARG_BOOL,
        .required = false,
    },
    {
        .id = "restart_on_success",
        .type = TELE_COMMAND_ARG_BOOL,
        .required = false,
    },
};

static const char *json_string_arg(const cJSON *args, const char *id, bool required)
{
    const cJSON *item = cJSON_IsObject(args) ? cJSON_GetObjectItemCaseSensitive(args, id) : NULL;
    if (!cJSON_IsString(item) || !item->valuestring || item->valuestring[0] == '\0') {
        return required ? NULL : "";
    }
    return item->valuestring;
}

static bool json_bool_arg(const cJSON *args, const char *id, bool default_value)
{
    const cJSON *item = cJSON_IsObject(args) ? cJSON_GetObjectItemCaseSensitive(args, id) : NULL;
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return default_value;
}

static bool json_has_bool_arg(const cJSON *args, const char *id)
{
    const cJSON *item = cJSON_IsObject(args) ? cJSON_GetObjectItemCaseSensitive(args, id) : NULL;
    return cJSON_IsBool(item);
}

static esp_err_t build_request(const cJSON *args,
                               tele_artifact_request_t *request,
                               const char **out_error)
{
    const char *artifact_type = json_string_arg(args, "artifact_type", true);
    const char *manifest_url = json_string_arg(args, "manifest_url", true);
    const char *channel = json_string_arg(args, "channel", false);

    if (!artifact_type) {
        *out_error = "missing_artifact_type";
        return ESP_ERR_INVALID_ARG;
    }
    if (!manifest_url) {
        *out_error = "missing_manifest_url";
        return ESP_ERR_INVALID_ARG;
    }

    *request = (tele_artifact_request_t) {
        .artifact_type = artifact_type,
        .manifest_url = manifest_url,
        .channel = channel[0] != '\0' ? channel : NULL,
        .allow_same_version = json_bool_arg(args, "allow_same_version", false),
        .restart_on_success = json_bool_arg(args, "restart_on_success", false),
    };
    return ESP_OK;
}

static cJSON *build_artifact_json(const tele_manifest_artifact_t *artifact)
{
    cJSON *json = cJSON_CreateObject();
    if (!json || !artifact) {
        cJSON_Delete(json);
        return NULL;
    }

    cJSON_AddStringToObject(json, "artifact_type", artifact->artifact_type);
    cJSON_AddStringToObject(json, "channel", artifact->channel);
    cJSON_AddStringToObject(json, "target_version", artifact->version);
    cJSON_AddStringToObject(json, "build_id", artifact->build_id);
    cJSON_AddNumberToObject(json, "size", (double)artifact->size);
    cJSON_AddBoolToObject(json, "critical", artifact->critical);
    if (artifact->url_count > 0) {
        cJSON_AddStringToObject(json, "artifact_url", artifact->urls[0]);
    }
    return json;
}

static esp_err_t handle_artifact_check_command(const cJSON *args,
                                               cJSON **out_result,
                                               const char **out_error)
{
    tele_artifact_request_t request = {0};
    tele_artifact_check_result_t result = {0};

    esp_err_t err = build_request(args, &request, out_error);
    if (err != ESP_OK) {
        return err;
    }

    const tele_artifact_handler_t *handler = tele_artifacts_find(request.artifact_type);
    if (!handler) {
        *out_error = "unknown_artifact_type";
        return ESP_ERR_NOT_FOUND;
    }

    err = tele_artifacts_check(&request, &result);
    if (err != ESP_OK) {
        *out_error = "artifact_check_failed";
        return err;
    }

    cJSON *json = build_artifact_json(&result.artifact);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(json, "current_version", result.current_version);
    cJSON_AddBoolToObject(json, "available", result.available);
    cJSON_AddStringToObject(json, "mode", mode_name(handler->mode));
    *out_result = json;
    return ESP_OK;
}

static esp_err_t handle_artifact_apply_command(const cJSON *args,
                                               cJSON **out_result,
                                               const char **out_error)
{
    tele_artifact_request_t request = {0};
    tele_artifact_apply_result_t result = {0};

    esp_err_t err = build_request(args, &request, out_error);
    if (err != ESP_OK) {
        return err;
    }

    const tele_artifact_handler_t *handler = tele_artifacts_find(request.artifact_type);
    if (!handler) {
        *out_error = "unknown_artifact_type";
        return ESP_ERR_NOT_FOUND;
    }
    if (!json_has_bool_arg(args, "restart_on_success")) {
        request.restart_on_success = handler->default_restart_on_success;
    }

    err = tele_artifacts_apply(&request, &result);
    if (err != ESP_OK) {
        *out_error = "artifact_apply_failed";
        return err;
    }

    cJSON *json = cJSON_CreateObject();
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(json, "artifact_type", request.artifact_type);
    cJSON_AddStringToObject(json, "mode", mode_name(handler->mode));
    cJSON_AddBoolToObject(json, "started_async", result.started_async);
    cJSON_AddNumberToObject(json, "result", (double)result.run.result);
    cJSON_AddStringToObject(json, "selected_url", result.run.selected_url);
    cJSON_AddNumberToObject(json, "bytes_received", (double)result.run.bytes_received);
    cJSON_AddStringToObject(json, "message", result.run.message);
    *out_result = json;
    return ESP_OK;
}

static esp_err_t handle_artifact_command(const char *cmd_name,
                                         const cJSON *args,
                                         cJSON **out_result,
                                         const char **out_error,
                                         uint32_t required_flags,
                                         void *ctx)
{
    (void)required_flags;
    (void)ctx;

    if (!cmd_name || !out_result || !out_error) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_result = NULL;
    *out_error = NULL;

    if (strcmp(cmd_name, "artifact/check") == 0) {
        return handle_artifact_check_command(args, out_result, out_error);
    }
    if (strcmp(cmd_name, "artifact/apply") == 0) {
        return handle_artifact_apply_command(args, out_result, out_error);
    }

    *out_error = "unknown_artifact_command";
    return ESP_ERR_NOT_FOUND;
}

static const tele_command_t artifact_commands[] = {
    {
        .name = "artifact/check",
        .label = "Consultar artefato",
        .description = "Consulta um manifest para um tipo de artefato registrado.",
        .group = "updates",
        .flags = TELE_COMMAND_FLAG_MQTT | TELE_COMMAND_FLAG_WEB,
        .args = artifact_command_args,
        .arg_count = sizeof(artifact_command_args) / sizeof(artifact_command_args[0]),
        .handler = handle_artifact_command,
    },
    {
        .name = "artifact/apply",
        .label = "Aplicar artefato",
        .description = "Aplica um artefato por manifest usando o handler registrado.",
        .group = "updates",
        .flags = TELE_COMMAND_FLAG_MQTT | TELE_COMMAND_FLAG_WEB | TELE_COMMAND_FLAG_MUTATING,
        .args = artifact_command_args,
        .arg_count = sizeof(artifact_command_args) / sizeof(artifact_command_args[0]),
        .handler = handle_artifact_command,
    },
};

esp_err_t tele_artifacts_register_commands(void)
{
    esp_err_t err = tele_commands_register(artifact_commands,
                                           sizeof(artifact_commands) / sizeof(artifact_commands[0]));
    return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
}

#endif
