#include "tele_status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TELE_STATUS_MAX_REGISTERED_FIELDS 64

static const tele_status_field_t *s_fields[TELE_STATUS_MAX_REGISTERED_FIELDS];
static size_t s_field_count;

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

#ifdef TELE_STATUS_HOST_TEST
typedef enum {
    HOST_JSON_BOOL,
    HOST_JSON_NUMBER,
    HOST_JSON_STRING,
} host_json_type_t;

typedef struct {
    char name[TELE_STATUS_ID_MAX_LEN + 1];
    host_json_type_t type;
    bool boolean;
    double number;
    const char *string;
} host_json_pair_t;

struct cJSON {
    host_json_pair_t pairs[TELE_STATUS_MAX_REGISTERED_FIELDS];
    size_t count;
};

cJSON *cJSON_CreateObject(void)
{
    return calloc(1, sizeof(cJSON));
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

char *cJSON_PrintUnformatted(const cJSON *item)
{
    char *buffer = NULL;
    size_t offset = 0;
    size_t capacity = 1024;

    if (!item) {
        return NULL;
    }

    buffer = malloc(capacity);
    if (!buffer) {
        return NULL;
    }

    buffer[offset++] = '{';
    for (size_t i = 0; i < item->count; ++i) {
        const host_json_pair_t *pair = &item->pairs[i];
        int written = 0;
        if (offset + 128 >= capacity) {
            char *grown = realloc(buffer, capacity * 2);
            if (!grown) {
                free(buffer);
                return NULL;
            }
            buffer = grown;
            capacity *= 2;
        }
        if (i > 0) {
            buffer[offset++] = ',';
        }
        switch (pair->type) {
        case HOST_JSON_BOOL:
            written = snprintf(buffer + offset,
                               capacity - offset,
                               "\"%s\":%s",
                               pair->name,
                               pair->boolean ? "true" : "false");
            break;
        case HOST_JSON_NUMBER:
            written = snprintf(buffer + offset,
                               capacity - offset,
                               "\"%s\":%.0f",
                               pair->name,
                               pair->number);
            break;
        case HOST_JSON_STRING:
            written = snprintf(buffer + offset,
                               capacity - offset,
                               "\"%s\":\"%s\"",
                               pair->name,
                               pair->string);
            break;
        }
        if (written < 0) {
            free(buffer);
            return NULL;
        }
        offset += (size_t)written;
    }
    if (offset + 2 >= capacity) {
        char *grown = realloc(buffer, capacity + 2);
        if (!grown) {
            free(buffer);
            return NULL;
        }
        buffer = grown;
        capacity += 2;
    }
    buffer[offset++] = '}';
    buffer[offset] = '\0';
    return buffer;
}

void cJSON_Delete(cJSON *item)
{
    free(item);
}

void cJSON_free(void *object)
{
    free(object);
}
#endif
