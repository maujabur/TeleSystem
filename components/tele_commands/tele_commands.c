#include "tele_commands.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TELE_COMMANDS_MAX_REGISTERED 32
#define TELE_COMMANDS_MAX_ARGS 8
#define TELE_COMMANDS_MAX_SEEN_MUTATING_IDS 16
#define TELE_COMMANDS_CMD_ID_MAX_LEN 64
#define TELE_COMMANDS_REGISTRY_REVISION 1

static const tele_command_t *s_commands[TELE_COMMANDS_MAX_REGISTERED];
static size_t s_command_count;
static char s_seen_mutating_ids[TELE_COMMANDS_MAX_SEEN_MUTATING_IDS][TELE_COMMANDS_CMD_ID_MAX_LEN];
static size_t s_seen_mutating_count;

static cJSON *add_object_to_array(cJSON *array)
{
    cJSON *object = cJSON_CreateObject();
    if (!object) {
        return NULL;
    }
    if (!cJSON_AddItemToArray(array, object)) {
        cJSON_Delete(object);
        return NULL;
    }
    return object;
}

static const char *arg_type_name(tele_command_arg_type_t type)
{
    switch (type) {
    case TELE_COMMAND_ARG_ANY:
        return "any";
    case TELE_COMMAND_ARG_BOOL:
        return "bool";
    case TELE_COMMAND_ARG_I32:
        return "i32";
    case TELE_COMMAND_ARG_U32:
        return "u32";
    case TELE_COMMAND_ARG_STRING:
        return "string";
    case TELE_COMMAND_ARG_OBJECT:
        return "object";
    default:
        return "unknown";
    }
}

static bool command_is_valid(const tele_command_t *command)
{
    if (!command || !command->name || command->name[0] == '\0' ||
        strlen(command->name) > TELE_COMMAND_NAME_MAX_LEN) {
        return false;
    }
    if (command->arg_count > TELE_COMMANDS_MAX_ARGS) {
        return false;
    }
    if (command->arg_count > 0 && !command->args) {
        return false;
    }

    for (size_t i = 0; i < command->arg_count; ++i) {
        const tele_command_arg_t *arg = &command->args[i];
        if (!arg->id || arg->id[0] == '\0' || strlen(arg->id) > TELE_COMMAND_ARG_ID_MAX_LEN) {
            return false;
        }
    }
    return true;
}

esp_err_t tele_commands_register(const tele_command_t *commands, size_t command_count)
{
    if (!commands || command_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_command_count + command_count > TELE_COMMANDS_MAX_REGISTERED) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < command_count; ++i) {
        if (!command_is_valid(&commands[i])) {
            return ESP_ERR_INVALID_ARG;
        }
        if (tele_commands_find(commands[i].name)) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    for (size_t i = 0; i < command_count; ++i) {
        s_commands[s_command_count++] = &commands[i];
    }
    return ESP_OK;
}

const tele_command_t *tele_commands_find(const char *name)
{
    if (!name) {
        return NULL;
    }

    for (size_t i = 0; i < s_command_count; ++i) {
        if (s_commands[i] && s_commands[i]->name && strcmp(s_commands[i]->name, name) == 0) {
            return s_commands[i];
        }
    }
    return NULL;
}

static bool command_id_was_seen(const char *cmd_id)
{
    if (!cmd_id || cmd_id[0] == '\0') {
        return false;
    }

    for (size_t i = 0; i < s_seen_mutating_count; ++i) {
        if (strcmp(s_seen_mutating_ids[i], cmd_id) == 0) {
            return true;
        }
    }
    return false;
}

static void remember_command_id(const char *cmd_id)
{
    if (!cmd_id || cmd_id[0] == '\0') {
        return;
    }

    size_t slot = s_seen_mutating_count;
    if (slot >= TELE_COMMANDS_MAX_SEEN_MUTATING_IDS) {
        memmove(&s_seen_mutating_ids[0],
                &s_seen_mutating_ids[1],
                sizeof(s_seen_mutating_ids[0]) * (TELE_COMMANDS_MAX_SEEN_MUTATING_IDS - 1));
        slot = TELE_COMMANDS_MAX_SEEN_MUTATING_IDS - 1;
    } else {
        s_seen_mutating_count++;
    }

    snprintf(s_seen_mutating_ids[slot], sizeof(s_seen_mutating_ids[slot]), "%s", cmd_id);
}

static void response_set_error(tele_command_response_t *response, const char *error)
{
    if (!response) {
        return;
    }

    response->ok = false;
    response->error = error;
    response->result = NULL;
}

esp_err_t tele_commands_execute(const tele_command_request_t *request,
                                tele_command_response_t *response)
{
    const tele_command_t *command = NULL;
    const char *handler_error = NULL;
    esp_err_t err = ESP_OK;

    if (!request || !response || !request->name || request->name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    memset(response, 0, sizeof(*response));
    command = tele_commands_find(request->name);
    if (!command ||
        (request->required_flags != 0 &&
         (command->flags & request->required_flags) != request->required_flags)) {
        response_set_error(response, "unsupported_command");
        return ESP_OK;
    }

    if (!command->handler) {
        response_set_error(response, "unsupported_command");
        return ESP_OK;
    }

    if ((command->flags & TELE_COMMAND_FLAG_MUTATING) != 0 &&
        command_id_was_seen(request->cmd_id)) {
        response_set_error(response, "duplicate_command");
        return ESP_OK;
    }

    err = command->handler(command->name,
                           request->args,
                           &response->result,
                           &handler_error,
                           request->required_flags,
                           command->ctx);
    if (err == ESP_OK) {
        response->ok = true;
        response->error = NULL;
        if ((command->flags & TELE_COMMAND_FLAG_MUTATING) != 0) {
            remember_command_id(request->cmd_id);
        }
        return ESP_OK;
    }

    response->ok = false;
    response->error = handler_error ? handler_error : "command_failed";
    return ESP_OK;
}

void tele_commands_response_cleanup(tele_command_response_t *response)
{
    if (!response) {
        return;
    }

    if (response->result) {
        cJSON_Delete(response->result);
    }
    memset(response, 0, sizeof(*response));
}

esp_err_t tele_commands_add_manifest_to_json(cJSON *root, uint32_t required_flags)
{
    cJSON *commands = NULL;

    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON_AddNumberToObject(root, "registry_revision", TELE_COMMANDS_REGISTRY_REVISION);
    commands = cJSON_AddArrayToObject(root, "commands");
    if (!commands) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < s_command_count; ++i) {
        const tele_command_t *command = s_commands[i];
        cJSON *item = NULL;
        cJSON *args = NULL;

        if (!command || (command->flags & required_flags) != required_flags) {
            continue;
        }

        item = add_object_to_array(commands);
        if (!item) {
            return ESP_ERR_NO_MEM;
        }

        cJSON_AddStringToObject(item, "name", command->name);
        if (command->label && command->label[0] != '\0') {
            cJSON_AddStringToObject(item, "label", command->label);
        }
        if (command->description && command->description[0] != '\0') {
            cJSON_AddStringToObject(item, "description", command->description);
        }
        if (command->group && command->group[0] != '\0') {
            cJSON_AddStringToObject(item, "group", command->group);
        }
        cJSON_AddBoolToObject(item, "mutating", (command->flags & TELE_COMMAND_FLAG_MUTATING) != 0);
        cJSON_AddBoolToObject(item, "reboot_required", (command->flags & TELE_COMMAND_FLAG_REBOOT_REQUIRED) != 0);
        cJSON_AddBoolToObject(item, "internal", (command->flags & TELE_COMMAND_FLAG_INTERNAL) != 0);

        args = cJSON_AddArrayToObject(item, "args");
        if (!args) {
            return ESP_ERR_NO_MEM;
        }

        for (size_t arg_index = 0; arg_index < command->arg_count; ++arg_index) {
            const tele_command_arg_t *arg = &command->args[arg_index];
            cJSON *arg_item = add_object_to_array(args);
            if (!arg_item) {
                return ESP_ERR_NO_MEM;
            }

            cJSON_AddStringToObject(arg_item, "id", arg->id);
            cJSON_AddStringToObject(arg_item, "type", arg_type_name(arg->type));
            cJSON_AddBoolToObject(arg_item, "required", arg->required);

            if (arg->type == TELE_COMMAND_ARG_I32) {
                cJSON_AddNumberToObject(arg_item, "min", (double)arg->min.i32);
                cJSON_AddNumberToObject(arg_item, "max", (double)arg->max.i32);
            } else if (arg->type == TELE_COMMAND_ARG_U32) {
                cJSON_AddNumberToObject(arg_item, "min", (double)arg->min.u32);
                cJSON_AddNumberToObject(arg_item, "max", (double)arg->max.u32);
            } else if (arg->type == TELE_COMMAND_ARG_STRING) {
                if (arg->min_len > 0) {
                    cJSON_AddNumberToObject(arg_item, "min_len", (double)arg->min_len);
                }
                if (arg->max_len > 0) {
                    cJSON_AddNumberToObject(arg_item, "max_len", (double)arg->max_len);
                }
            }
        }
    }

    return ESP_OK;
}

#ifdef TELE_COMMANDS_HOST_TEST
typedef enum {
    HOST_JSON_ARRAY,
    HOST_JSON_BOOL,
    HOST_JSON_NUMBER,
    HOST_JSON_OBJECT,
    HOST_JSON_STRING,
} host_json_type_t;

typedef struct {
    char name[TELE_COMMAND_NAME_MAX_LEN + 1];
    host_json_type_t type;
    bool boolean;
    double number;
    struct cJSON *child;
    const char *string;
} host_json_pair_t;

struct cJSON {
    host_json_pair_t pairs[TELE_COMMANDS_MAX_REGISTERED + TELE_COMMANDS_MAX_ARGS + 8];
    size_t count;
    bool is_array;
};

cJSON *cJSON_CreateObject(void)
{
    return calloc(1, sizeof(cJSON));
}

cJSON *cJSON_AddArrayToObject(cJSON *object, const char *name)
{
    if (!object || !name || object->count >= (sizeof(object->pairs) / sizeof(object->pairs[0]))) {
        return NULL;
    }
    cJSON *array = calloc(1, sizeof(cJSON));
    if (!array) {
        return NULL;
    }
    array->is_array = true;
    host_json_pair_t *pair = &object->pairs[object->count++];
    snprintf(pair->name, sizeof(pair->name), "%s", name);
    pair->type = HOST_JSON_ARRAY;
    pair->child = array;
    return array;
}

bool cJSON_AddItemToArray(cJSON *array, cJSON *item)
{
    if (!array || !array->is_array || !item || array->count >= (sizeof(array->pairs) / sizeof(array->pairs[0]))) {
        return false;
    }
    host_json_pair_t *pair = &array->pairs[array->count++];
    pair->type = HOST_JSON_OBJECT;
    pair->child = item;
    return true;
}

bool cJSON_AddBoolToObject(cJSON *object, const char *name, bool value)
{
    if (!object || !name || object->count >= (sizeof(object->pairs) / sizeof(object->pairs[0]))) {
        return false;
    }
    host_json_pair_t *pair = &object->pairs[object->count++];
    snprintf(pair->name, sizeof(pair->name), "%s", name);
    pair->type = HOST_JSON_BOOL;
    pair->boolean = value;
    return true;
}

double cJSON_AddNumberToObject(cJSON *object, const char *name, double value)
{
    if (!object || !name || object->count >= (sizeof(object->pairs) / sizeof(object->pairs[0]))) {
        return 0;
    }
    host_json_pair_t *pair = &object->pairs[object->count++];
    snprintf(pair->name, sizeof(pair->name), "%s", name);
    pair->type = HOST_JSON_NUMBER;
    pair->number = value;
    return value;
}

bool cJSON_AddStringToObject(cJSON *object, const char *name, const char *value)
{
    if (!object || !name || object->count >= (sizeof(object->pairs) / sizeof(object->pairs[0]))) {
        return false;
    }
    host_json_pair_t *pair = &object->pairs[object->count++];
    snprintf(pair->name, sizeof(pair->name), "%s", name);
    pair->type = HOST_JSON_STRING;
    pair->string = value ? value : "";
    return true;
}

static bool append_json(char **buffer, size_t *offset, size_t *capacity, const cJSON *item);

static bool ensure_json_capacity(char **buffer, size_t *capacity, size_t needed)
{
    if (needed < *capacity) {
        return true;
    }
    size_t new_capacity = *capacity;
    while (needed >= new_capacity) {
        new_capacity *= 2;
    }
    char *grown = realloc(*buffer, new_capacity);
    if (!grown) {
        return false;
    }
    *buffer = grown;
    *capacity = new_capacity;
    return true;
}

static bool append_format(char **buffer, size_t *offset, size_t *capacity, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int written = vsnprintf(*buffer + *offset, *capacity - *offset, format, args);
    va_end(args);

    if (written < 0) {
        return false;
    }
    if (*offset + (size_t)written >= *capacity) {
        if (!ensure_json_capacity(buffer, capacity, *offset + (size_t)written + 1)) {
            return false;
        }
        va_start(args, format);
        written = vsnprintf(*buffer + *offset, *capacity - *offset, format, args);
        va_end(args);
        if (written < 0) {
            return false;
        }
    }
    *offset += (size_t)written;
    return true;
}

static bool append_json(char **buffer, size_t *offset, size_t *capacity, const cJSON *item)
{
    if (!item) {
        return false;
    }

    if (!ensure_json_capacity(buffer, capacity, *offset + 2)) {
        return false;
    }

    (*buffer)[(*offset)++] = item->is_array ? '[' : '{';
    for (size_t i = 0; i < item->count; ++i) {
        const host_json_pair_t *pair = &item->pairs[i];
        if (i > 0) {
            (*buffer)[(*offset)++] = ',';
        }
        if (!item->is_array && !append_format(buffer, offset, capacity, "\"%s\":", pair->name)) {
            return false;
        }
        switch (pair->type) {
        case HOST_JSON_ARRAY:
        case HOST_JSON_OBJECT:
            if (!append_json(buffer, offset, capacity, pair->child)) {
                return false;
            }
            break;
        case HOST_JSON_BOOL:
            if (!append_format(buffer, offset, capacity, "%s", pair->boolean ? "true" : "false")) {
                return false;
            }
            break;
        case HOST_JSON_NUMBER:
            if (!append_format(buffer, offset, capacity, "%.0f", pair->number)) {
                return false;
            }
            break;
        case HOST_JSON_STRING:
            if (!append_format(buffer, offset, capacity, "\"%s\"", pair->string)) {
                return false;
            }
            break;
        }
    }
    if (!ensure_json_capacity(buffer, capacity, *offset + 2)) {
        return false;
    }
    (*buffer)[(*offset)++] = item->is_array ? ']' : '}';
    (*buffer)[*offset] = '\0';
    return true;
}

char *cJSON_PrintUnformatted(const cJSON *item)
{
    size_t offset = 0;
    size_t capacity = 1024;
    char *buffer = malloc(capacity);
    if (!buffer) {
        return NULL;
    }

    if (!append_json(&buffer, &offset, &capacity, item)) {
        free(buffer);
        return NULL;
    }
    return buffer;
}

void cJSON_Delete(cJSON *item)
{
    if (!item) {
        return;
    }
    for (size_t i = 0; i < item->count; ++i) {
        if (item->pairs[i].child) {
            cJSON_Delete(item->pairs[i].child);
        }
    }
    free(item);
}

void cJSON_free(void *object)
{
    free(object);
}
#endif
