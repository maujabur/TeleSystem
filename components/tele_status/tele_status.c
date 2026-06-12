#include "tele_status.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TELE_STATUS_MAX_REGISTERED_FIELDS 64
#define TELE_STATUS_REGISTRY_REVISION 1

static const tele_status_field_t *s_fields[TELE_STATUS_MAX_REGISTERED_FIELDS];
static size_t s_field_count;

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

static const char *type_name(tele_status_type_t type)
{
    switch (type) {
    case TELE_STATUS_TYPE_BOOL:
        return "bool";
    case TELE_STATUS_TYPE_I32:
        return "i32";
    case TELE_STATUS_TYPE_U32:
        return "u32";
    case TELE_STATUS_TYPE_STRING:
        return "string";
    default:
        return "unknown";
    }
}

static void add_flag_if_set(cJSON *flags, uint32_t field_flags, uint32_t flag, const char *name)
{
    if ((field_flags & flag) == flag) {
        cJSON *entry = add_object_to_array(flags);
        if (entry) {
            cJSON_AddStringToObject(entry, "flag", name);
        }
    }
}

static bool field_has_reader(const tele_status_field_t *field)
{
    if (!field) {
        return false;
    }

    switch (field->type) {
    case TELE_STATUS_TYPE_BOOL:
        return field->read.boolean != NULL;
    case TELE_STATUS_TYPE_I32:
        return field->read.i32 != NULL;
    case TELE_STATUS_TYPE_U32:
        return field->read.u32 != NULL;
    case TELE_STATUS_TYPE_STRING:
        return field->read.string != NULL;
    default:
        return false;
    }
}

esp_err_t tele_status_register_fields(const tele_status_field_t *fields, size_t field_count)
{
    if (!fields || field_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_field_count + field_count > TELE_STATUS_MAX_REGISTERED_FIELDS) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < field_count; ++i) {
        const tele_status_field_t *field = &fields[i];
        if (!field->id || field->id[0] == '\0' || strlen(field->id) > TELE_STATUS_ID_MAX_LEN) {
            return ESP_ERR_INVALID_ARG;
        }
        if (!field_has_reader(field)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (tele_status_find_field(field->id)) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    for (size_t i = 0; i < field_count; ++i) {
        s_fields[s_field_count++] = &fields[i];
    }
    return ESP_OK;
}

const tele_status_field_t *tele_status_find_field(const char *id)
{
    if (!id) {
        return NULL;
    }

    for (size_t i = 0; i < s_field_count; ++i) {
        if (s_fields[i] && s_fields[i]->id && strcmp(s_fields[i]->id, id) == 0) {
            return s_fields[i];
        }
    }

    return NULL;
}

esp_err_t tele_status_add_fields_to_json(cJSON *root, uint32_t required_flags)
{
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < s_field_count; ++i) {
        const tele_status_field_t *field = s_fields[i];
        if (!field || (field->flags & required_flags) != required_flags) {
            continue;
        }

        switch (field->type) {
        case TELE_STATUS_TYPE_BOOL:
            cJSON_AddBoolToObject(root, field->id, field->read.boolean(field->ctx));
            break;
        case TELE_STATUS_TYPE_I32:
            cJSON_AddNumberToObject(root, field->id, (double)field->read.i32(field->ctx));
            break;
        case TELE_STATUS_TYPE_U32:
            cJSON_AddNumberToObject(root, field->id, (double)field->read.u32(field->ctx));
            break;
        case TELE_STATUS_TYPE_STRING: {
            const char *value = field->read.string(field->ctx);
            cJSON_AddStringToObject(root, field->id, value ? value : "");
            break;
        }
        default:
            return ESP_ERR_INVALID_ARG;
        }
    }

    return ESP_OK;
}

esp_err_t tele_status_add_manifest_to_json(cJSON *root, uint32_t required_flags)
{
    cJSON *fields = NULL;

    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON_AddNumberToObject(root, "registry_revision", TELE_STATUS_REGISTRY_REVISION);
    fields = cJSON_AddArrayToObject(root, "fields");
    if (!fields) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < s_field_count; ++i) {
        const tele_status_field_t *field = s_fields[i];
        cJSON *item = NULL;
        cJSON *flags = NULL;

        if (!field || (field->flags & required_flags) != required_flags) {
            continue;
        }

        item = add_object_to_array(fields);
        if (!item) {
            return ESP_ERR_NO_MEM;
        }

        cJSON_AddStringToObject(item, "id", field->id);
        cJSON_AddStringToObject(item, "type", type_name(field->type));
        if (field->unit && field->unit[0] != '\0') {
            cJSON_AddStringToObject(item, "unit", field->unit);
        }

        flags = cJSON_AddArrayToObject(item, "flags");
        if (!flags) {
            return ESP_ERR_NO_MEM;
        }
        add_flag_if_set(flags, field->flags, TELE_STATUS_FLAG_STATE, "state");
        add_flag_if_set(flags, field->flags, TELE_STATUS_FLAG_HEARTBEAT, "heartbeat");
        add_flag_if_set(flags, field->flags, TELE_STATUS_FLAG_TECHNICAL, "technical");
        add_flag_if_set(flags, field->flags, TELE_STATUS_FLAG_MQTT, "mqtt");
        add_flag_if_set(flags, field->flags, TELE_STATUS_FLAG_WEB, "web");
        add_flag_if_set(flags, field->flags, TELE_STATUS_FLAG_SENSITIVE, "sensitive");
    }

    return ESP_OK;
}

#ifdef TELE_STATUS_HOST_TEST
typedef enum {
    HOST_JSON_ARRAY,
    HOST_JSON_BOOL,
    HOST_JSON_NUMBER,
    HOST_JSON_OBJECT,
    HOST_JSON_STRING,
} host_json_type_t;

typedef struct {
    char name[TELE_STATUS_ID_MAX_LEN + 1];
    host_json_type_t type;
    bool boolean;
    double number;
    struct cJSON *child;
    const char *string;
} host_json_pair_t;

struct cJSON {
    host_json_pair_t pairs[TELE_STATUS_MAX_REGISTERED_FIELDS];
    size_t count;
    bool is_array;
};

cJSON *cJSON_CreateObject(void)
{
    return calloc(1, sizeof(cJSON));
}

cJSON *cJSON_AddArrayToObject(cJSON *object, const char *name)
{
    if (!object || !name || object->count >= TELE_STATUS_MAX_REGISTERED_FIELDS) {
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
    if (!array || !array->is_array || !item || array->count >= TELE_STATUS_MAX_REGISTERED_FIELDS) {
        return false;
    }
    host_json_pair_t *pair = &array->pairs[array->count++];
    pair->type = HOST_JSON_OBJECT;
    pair->child = item;
    return true;
}

bool cJSON_AddBoolToObject(cJSON *object, const char *name, bool value)
{
    if (!object || !name || object->count >= TELE_STATUS_MAX_REGISTERED_FIELDS) {
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
    if (!object || !name || object->count >= TELE_STATUS_MAX_REGISTERED_FIELDS) {
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
    if (!object || !name || object->count >= TELE_STATUS_MAX_REGISTERED_FIELDS) {
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
