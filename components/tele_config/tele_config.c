#include "tele_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TELE_CONFIG_HOST_TEST
#include "nvs.h"
#endif

#define TELE_CONFIG_MAX_REGISTERED_FIELDS 64
#define TELE_CONFIG_NVS_NAMESPACE "tele_config"
#define TELE_CONFIG_REGISTRY_REVISION 1

typedef struct {
    const tele_config_field_t *field;
    tele_config_apply_cb_t apply_cb;
    void *apply_ctx;
} tele_config_registry_entry_t;

static tele_config_registry_entry_t s_entries[TELE_CONFIG_MAX_REGISTERED_FIELDS];
static size_t s_field_count;

esp_err_t tele_config_reset_override(const char *id);

static tele_config_registry_entry_t *find_entry(const char *id)
{
    if (!id) {
        return NULL;
    }

    for (size_t i = 0; i < s_field_count; ++i) {
        if (s_entries[i].field &&
            s_entries[i].field->id &&
            strcmp(s_entries[i].field->id, id) == 0) {
            return &s_entries[i];
        }
    }

    return NULL;
}

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

static const char *type_name(tele_config_type_t type)
{
    switch (type) {
    case TELE_CONFIG_TYPE_BOOL:
        return "bool";
    case TELE_CONFIG_TYPE_I32:
        return "i32";
    case TELE_CONFIG_TYPE_U32:
        return "u32";
    case TELE_CONFIG_TYPE_STRING:
        return "string";
    case TELE_CONFIG_TYPE_ENUM:
        return "enum";
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

static esp_err_t add_config_value_to_json(cJSON *item,
                                          const char *name,
                                          tele_config_type_t type,
                                          const tele_config_value_t *value,
                                          bool hide)
{
    if (!item || !name || !value) {
        return ESP_ERR_INVALID_ARG;
    }

    if (hide) {
        char hidden_name[32] = {0};
        if (snprintf(hidden_name, sizeof(hidden_name), "%s_hidden", name) >= (int)sizeof(hidden_name)) {
            return ESP_ERR_INVALID_SIZE;
        }
        cJSON_AddBoolToObject(item, hidden_name, true);
        return ESP_OK;
    }

    switch (type) {
    case TELE_CONFIG_TYPE_BOOL:
        cJSON_AddBoolToObject(item, name, value->boolean);
        return ESP_OK;
    case TELE_CONFIG_TYPE_I32:
    case TELE_CONFIG_TYPE_ENUM:
        cJSON_AddNumberToObject(item, name, (double)value->i32);
        return ESP_OK;
    case TELE_CONFIG_TYPE_U32:
        cJSON_AddNumberToObject(item, name, (double)value->u32);
        return ESP_OK;
    case TELE_CONFIG_TYPE_STRING:
        cJSON_AddStringToObject(item, name, value->string ? value->string : "");
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t add_field_limits_to_json(cJSON *item, const tele_config_field_t *field)
{
    if (!item || !field) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (field->type) {
    case TELE_CONFIG_TYPE_I32:
    case TELE_CONFIG_TYPE_ENUM:
        cJSON_AddNumberToObject(item, "min", (double)field->min.i32);
        cJSON_AddNumberToObject(item, "max", (double)field->max.i32);
        return ESP_OK;
    case TELE_CONFIG_TYPE_U32:
        cJSON_AddNumberToObject(item, "min", (double)field->min.u32);
        cJSON_AddNumberToObject(item, "max", (double)field->max.u32);
        return ESP_OK;
    case TELE_CONFIG_TYPE_STRING:
        cJSON_AddNumberToObject(item, "min_len", (double)field->min_len);
        cJSON_AddNumberToObject(item, "max_len", (double)field->max_len);
        return ESP_OK;
    case TELE_CONFIG_TYPE_BOOL:
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t add_enum_choices_to_json(cJSON *item, const tele_config_field_t *field)
{
    cJSON *choices = NULL;

    if (!item || !field) {
        return ESP_ERR_INVALID_ARG;
    }
    if (field->type != TELE_CONFIG_TYPE_ENUM || !field->choices || field->choice_count == 0) {
        return ESP_OK;
    }

    choices = cJSON_AddArrayToObject(item, "choices");
    if (!choices) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < field->choice_count; ++i) {
        cJSON *choice = add_object_to_array(choices);
        if (!choice) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddNumberToObject(choice, "value", (double)field->choices[i].value);
        cJSON_AddStringToObject(choice, "label", field->choices[i].label ? field->choices[i].label : "");
    }

    return ESP_OK;
}

static esp_err_t validate_nvs_key(const char *nvs_key)
{
    if (!nvs_key || nvs_key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(nvs_key) > TELE_CONFIG_NVS_KEY_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t copy_nvs_key(const tele_config_field_t *field, char *out, size_t out_size)
{
    if (!field || !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = validate_nvs_key(field->nvs_key);
    if (err != ESP_OK) {
        return err;
    }
    if (strlen(field->nvs_key) >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    strcpy(out, field->nvs_key);
    return ESP_OK;
}

static esp_err_t copy_string_value(char *buffer,
                                   size_t buffer_size,
                                   const char *value,
                                   tele_config_value_t *out_value)
{
    if (!buffer || buffer_size == 0 || !value || !out_value) {
        return ESP_ERR_INVALID_ARG;
    }
    if (snprintf(buffer, buffer_size, "%s", value) >= (int)buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    out_value->string = buffer;
    return ESP_OK;
}

static esp_err_t field_default_value(const tele_config_field_t *field,
                                     tele_config_value_t *out_value,
                                     char *string_buffer,
                                     size_t string_buffer_size)
{
    if (!field || !out_value) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (field->type) {
    case TELE_CONFIG_TYPE_BOOL:
        out_value->boolean = field->default_value.boolean;
        return ESP_OK;
    case TELE_CONFIG_TYPE_I32:
    case TELE_CONFIG_TYPE_ENUM:
        out_value->i32 = field->default_value.i32;
        return ESP_OK;
    case TELE_CONFIG_TYPE_U32:
        out_value->u32 = field->default_value.u32;
        return ESP_OK;
    case TELE_CONFIG_TYPE_STRING:
        return copy_string_value(string_buffer,
                                 string_buffer_size,
                                 field->default_value.string,
                                 out_value);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t validate_field_default(const tele_config_field_t *field)
{
    tele_config_value_t value = {0};

    if (!field) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (field->type) {
    case TELE_CONFIG_TYPE_BOOL:
        value.boolean = field->default_value.boolean;
        break;
    case TELE_CONFIG_TYPE_I32:
    case TELE_CONFIG_TYPE_ENUM:
        value.i32 = field->default_value.i32;
        break;
    case TELE_CONFIG_TYPE_U32:
        value.u32 = field->default_value.u32;
        break;
    case TELE_CONFIG_TYPE_STRING:
        value.string = field->default_value.string;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    return tele_config_validate_value(field, &value);
}

static esp_err_t store_override_value(const tele_config_field_t *field, const tele_config_value_t *value)
{
    if (!field || !value) {
        return ESP_ERR_INVALID_ARG;
    }

#ifdef TELE_CONFIG_HOST_TEST
    return ESP_OK;
#else
    char key[TELE_CONFIG_NVS_KEY_MAX_LEN + 1] = {0};
    esp_err_t err = copy_nvs_key(field, key, sizeof(key));
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle = 0;
    err = nvs_open(TELE_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    switch (field->type) {
    case TELE_CONFIG_TYPE_BOOL:
        err = nvs_set_u8(handle, key, value->boolean ? 1 : 0);
        break;
    case TELE_CONFIG_TYPE_I32:
    case TELE_CONFIG_TYPE_ENUM:
        err = nvs_set_i32(handle, key, value->i32);
        break;
    case TELE_CONFIG_TYPE_U32:
        err = nvs_set_u32(handle, key, value->u32);
        break;
    case TELE_CONFIG_TYPE_STRING:
        err = nvs_set_str(handle, key, value->string);
        break;
    default:
        err = ESP_ERR_INVALID_ARG;
        break;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
#endif
}

esp_err_t tele_config_validate_value(const tele_config_field_t *field,
                                     const tele_config_value_t *value)
{
    if (!field || !value || !field->id || field->id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    switch (field->type) {
    case TELE_CONFIG_TYPE_BOOL:
        return ESP_OK;
    case TELE_CONFIG_TYPE_I32:
    case TELE_CONFIG_TYPE_ENUM:
        if (value->i32 < field->min.i32 || value->i32 > field->max.i32) {
            return ESP_ERR_INVALID_ARG;
        }
        return ESP_OK;
    case TELE_CONFIG_TYPE_U32:
        if (value->u32 < field->min.u32 || value->u32 > field->max.u32) {
            return ESP_ERR_INVALID_ARG;
        }
        return ESP_OK;
    case TELE_CONFIG_TYPE_STRING: {
        if (!value->string) {
            return ESP_ERR_INVALID_ARG;
        }
        size_t len = strlen(value->string);
        if (len < field->min_len) {
            return ESP_ERR_INVALID_ARG;
        }
        if (field->max_len > 0 && len > field->max_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        return ESP_OK;
    }
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t tele_config_register_fields(const tele_config_field_t *fields, size_t field_count)
{
    if (!fields || field_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_field_count + field_count > TELE_CONFIG_MAX_REGISTERED_FIELDS) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < field_count; ++i) {
        if (!fields[i].id || fields[i].id[0] == '\0') {
            return ESP_ERR_INVALID_ARG;
        }
        esp_err_t err = validate_nvs_key(fields[i].nvs_key);
        if (err != ESP_OK) {
            return err;
        }
        err = validate_field_default(&fields[i]);
        if (err != ESP_OK) {
            return err;
        }
        if (tele_config_find_field(fields[i].id)) {
            return ESP_ERR_INVALID_STATE;
        }
        s_entries[s_field_count++].field = &fields[i];
    }
    return ESP_OK;
}

const tele_config_field_t *tele_config_find_field(const char *id)
{
    tele_config_registry_entry_t *entry = find_entry(id);
    return entry ? entry->field : NULL;
}

esp_err_t tele_config_set_apply_handler(const char *id, tele_config_apply_cb_t apply_cb, void *ctx)
{
    tele_config_registry_entry_t *entry = find_entry(id);
    if (!entry) {
        return ESP_ERR_INVALID_ARG;
    }

    entry->apply_cb = apply_cb;
    entry->apply_ctx = ctx;
    return ESP_OK;
}

esp_err_t tele_config_apply_value(const char *id, const tele_config_value_t *value)
{
    tele_config_registry_entry_t *entry = find_entry(id);
    if (!entry || !entry->field || !value) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = tele_config_validate_value(entry->field, value);
    if (err != ESP_OK) {
        return err;
    }
    if (!entry->apply_cb) {
        return ESP_OK;
    }

    return entry->apply_cb(entry->field, value, entry->apply_ctx);
}

esp_err_t tele_config_update_value(const char *id,
                                   const tele_config_value_t *value,
                                   tele_config_update_result_t *out_result)
{
    tele_config_registry_entry_t *entry = find_entry(id);
    tele_config_update_result_t result = {0};

    if (out_result) {
        *out_result = result;
    }
    if (!entry || !entry->field || !value) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((entry->field->flags & TELE_CONFIG_FLAG_READ_ONLY) != 0) {
        return ESP_ERR_INVALID_STATE;
    }

    result.requires_reboot = (entry->field->flags & TELE_CONFIG_FLAG_REBOOT_REQUIRED) != 0;

    esp_err_t err = tele_config_validate_value(entry->field, value);
    if (err != ESP_OK) {
        if (out_result) {
            *out_result = result;
        }
        return err;
    }

    if (entry->apply_cb) {
        err = entry->apply_cb(entry->field, value, entry->apply_ctx);
        if (err != ESP_OK) {
            if (out_result) {
                *out_result = result;
            }
            return err;
        }
        result.applied = true;
    }

    err = store_override_value(entry->field, value);
    if (err != ESP_OK) {
        if (out_result) {
            *out_result = result;
        }
        return err;
    }
    result.stored = true;

    if (out_result) {
        *out_result = result;
    }
    return ESP_OK;
}

esp_err_t tele_config_reset_value(const char *id, tele_config_update_result_t *out_result)
{
    tele_config_registry_entry_t *entry = find_entry(id);
    tele_config_update_result_t result = {0};
    tele_config_value_t default_value = {0};
    char default_string[TELE_CONFIG_STRING_MAX_LEN + 1] = {0};

    if (out_result) {
        *out_result = result;
    }
    if (!entry || !entry->field) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((entry->field->flags & TELE_CONFIG_FLAG_READ_ONLY) != 0) {
        return ESP_ERR_INVALID_STATE;
    }

    result.requires_reboot = (entry->field->flags & TELE_CONFIG_FLAG_REBOOT_REQUIRED) != 0;

    esp_err_t err = tele_config_reset_override(entry->field->id);
    if (err != ESP_OK) {
        if (out_result) {
            *out_result = result;
        }
        return err;
    }

    if (entry->apply_cb) {
        err = field_default_value(entry->field, &default_value, default_string, sizeof(default_string));
        if (err != ESP_OK) {
            if (out_result) {
                *out_result = result;
            }
            return err;
        }
        err = entry->apply_cb(entry->field, &default_value, entry->apply_ctx);
        if (err != ESP_OK) {
            if (out_result) {
                *out_result = result;
            }
            return err;
        }
        result.applied = true;
    }

    if (out_result) {
        *out_result = result;
    }
    return ESP_OK;
}

esp_err_t tele_config_get_effective(const char *id,
                                    tele_config_value_t *out_value,
                                    char *string_buffer,
                                    size_t string_buffer_size,
                                    bool *out_from_nvs)
{
    const tele_config_field_t *field = tele_config_find_field(id);
    if (!field || !out_value) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_from_nvs) {
        *out_from_nvs = false;
    }

#ifdef TELE_CONFIG_HOST_TEST
    return field_default_value(field, out_value, string_buffer, string_buffer_size);
#else
    char key[TELE_CONFIG_NVS_KEY_MAX_LEN + 1] = {0};
    esp_err_t err = copy_nvs_key(field, key, sizeof(key));
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle = 0;
    err = nvs_open(TELE_CONFIG_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return field_default_value(field, out_value, string_buffer, string_buffer_size);
    }
    if (err != ESP_OK) {
        return err;
    }

    tele_config_value_t candidate = {0};
    switch (field->type) {
    case TELE_CONFIG_TYPE_BOOL: {
        uint8_t stored = 0;
        err = nvs_get_u8(handle, key, &stored);
        candidate.boolean = stored != 0;
        break;
    }
    case TELE_CONFIG_TYPE_I32:
    case TELE_CONFIG_TYPE_ENUM:
        err = nvs_get_i32(handle, key, &candidate.i32);
        break;
    case TELE_CONFIG_TYPE_U32:
        err = nvs_get_u32(handle, key, &candidate.u32);
        break;
    case TELE_CONFIG_TYPE_STRING:
        if (!string_buffer || string_buffer_size == 0) {
            nvs_close(handle);
            return ESP_ERR_INVALID_ARG;
        }
        err = nvs_get_str(handle, key, string_buffer, &string_buffer_size);
        candidate.string = string_buffer;
        break;
    default:
        nvs_close(handle);
        return ESP_ERR_INVALID_ARG;
    }
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return field_default_value(field, out_value, string_buffer, string_buffer_size);
    }
    if (err != ESP_OK) {
        return err;
    }
    err = tele_config_validate_value(field, &candidate);
    if (err != ESP_OK) {
        return field_default_value(field, out_value, string_buffer, string_buffer_size);
    }

    *out_value = candidate;
    if (out_from_nvs) {
        *out_from_nvs = true;
    }
    return ESP_OK;
#endif
}

esp_err_t tele_config_add_manifest_to_json(cJSON *root, uint32_t required_flags)
{
    cJSON *fields = NULL;

    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON_AddNumberToObject(root, "registry_revision", TELE_CONFIG_REGISTRY_REVISION);
    fields = cJSON_AddArrayToObject(root, "fields");
    if (!fields) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < s_field_count; ++i) {
        const tele_config_registry_entry_t *registry_entry = &s_entries[i];
        const tele_config_field_t *field = registry_entry->field;
        cJSON *item = NULL;
        cJSON *flags = NULL;
        tele_config_value_t default_value = {0};
        tele_config_value_t effective_value = {0};
        bool from_nvs = false;
        char default_string[TELE_CONFIG_STRING_MAX_LEN + 1] = {0};
        char effective_string[TELE_CONFIG_STRING_MAX_LEN + 1] = {0};
        bool hide_value = false;
        esp_err_t err = ESP_OK;

        if (!field || (field->flags & required_flags) != required_flags) {
            continue;
        }

        item = add_object_to_array(fields);
        if (!item) {
            return ESP_ERR_NO_MEM;
        }

        hide_value = (field->flags & TELE_CONFIG_FLAG_SECRET) != 0;
        err = field_default_value(field, &default_value, default_string, sizeof(default_string));
        if (err != ESP_OK) {
            return err;
        }
        err = tele_config_get_effective(field->id,
                                        &effective_value,
                                        effective_string,
                                        sizeof(effective_string),
                                        &from_nvs);
        if (err != ESP_OK) {
            return err;
        }

        cJSON_AddStringToObject(item, "id", field->id);
        cJSON_AddStringToObject(item, "type", type_name(field->type));
        cJSON_AddStringToObject(item, "source", from_nvs ? "nvs" : "default");
        err = add_config_value_to_json(item, "default", field->type, &default_value, hide_value);
        if (err != ESP_OK) {
            return err;
        }
        err = add_config_value_to_json(item, "value", field->type, &effective_value, hide_value);
        if (err != ESP_OK) {
            return err;
        }
        err = add_field_limits_to_json(item, field);
        if (err != ESP_OK) {
            return err;
        }
        err = add_enum_choices_to_json(item, field);
        if (err != ESP_OK) {
            return err;
        }

        flags = cJSON_AddArrayToObject(item, "flags");
        if (!flags) {
            return ESP_ERR_NO_MEM;
        }
        add_flag_if_set(flags, field->flags, TELE_CONFIG_FLAG_WEB, "web");
        add_flag_if_set(flags, field->flags, TELE_CONFIG_FLAG_MQTT, "mqtt");
        add_flag_if_set(flags, field->flags, TELE_CONFIG_FLAG_SECRET, "secret");
        add_flag_if_set(flags, field->flags, TELE_CONFIG_FLAG_REBOOT_REQUIRED, "reboot_required");
        add_flag_if_set(flags, field->flags, TELE_CONFIG_FLAG_READ_ONLY, "read_only");
        if (registry_entry->apply_cb) {
            cJSON *entry = add_object_to_array(flags);
            if (entry) {
                cJSON_AddStringToObject(entry, "flag", "runtime_apply");
            }
        }
    }

    return ESP_OK;
}

esp_err_t tele_config_set_override(const char *id, const tele_config_value_t *value)
{
    const tele_config_field_t *field = tele_config_find_field(id);
    if (!field || !value) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((field->flags & TELE_CONFIG_FLAG_READ_ONLY) != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = tele_config_validate_value(field, value);
    if (err != ESP_OK) {
        return err;
    }

    return store_override_value(field, value);
}

esp_err_t tele_config_reset_override(const char *id)
{
    const tele_config_field_t *field = tele_config_find_field(id);
    if (!field) {
        return ESP_ERR_INVALID_ARG;
    }

#ifdef TELE_CONFIG_HOST_TEST
    return ESP_OK;
#else
    char key[TELE_CONFIG_NVS_KEY_MAX_LEN + 1] = {0};
    esp_err_t err = copy_nvs_key(field, key, sizeof(key));
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle = 0;
    err = nvs_open(TELE_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
#endif
}

#ifdef TELE_CONFIG_HOST_TEST
typedef enum {
    HOST_JSON_ARRAY,
    HOST_JSON_BOOL,
    HOST_JSON_NUMBER,
    HOST_JSON_OBJECT,
    HOST_JSON_STRING,
} host_json_type_t;

typedef struct {
    char name[TELE_CONFIG_ID_MAX_LEN + 1];
    host_json_type_t type;
    bool boolean;
    double number;
    struct cJSON *child;
    char string[TELE_CONFIG_STRING_MAX_LEN + 1];
} host_json_pair_t;

struct cJSON {
    host_json_pair_t pairs[TELE_CONFIG_MAX_REGISTERED_FIELDS];
    size_t count;
    bool is_array;
};

cJSON *cJSON_CreateObject(void)
{
    return calloc(1, sizeof(cJSON));
}

cJSON *cJSON_AddArrayToObject(cJSON *object, const char *name)
{
    if (!object || !name || object->count >= TELE_CONFIG_MAX_REGISTERED_FIELDS) {
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
    if (!array || !array->is_array || !item || array->count >= TELE_CONFIG_MAX_REGISTERED_FIELDS) {
        return false;
    }
    host_json_pair_t *pair = &array->pairs[array->count++];
    pair->type = HOST_JSON_OBJECT;
    pair->child = item;
    return true;
}

bool cJSON_AddBoolToObject(cJSON *object, const char *name, bool value)
{
    if (!object || !name || object->count >= TELE_CONFIG_MAX_REGISTERED_FIELDS) {
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
    if (!object || !name || object->count >= TELE_CONFIG_MAX_REGISTERED_FIELDS) {
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
    if (!object || !name || object->count >= TELE_CONFIG_MAX_REGISTERED_FIELDS) {
        return false;
    }
    host_json_pair_t *pair = &object->pairs[object->count++];
    snprintf(pair->name, sizeof(pair->name), "%s", name);
    pair->type = HOST_JSON_STRING;
    snprintf(pair->string, sizeof(pair->string), "%s", value ? value : "");
    return true;
}

static bool append_text(char **buffer, size_t *offset, size_t *capacity, const char *text)
{
    size_t len = strlen(text);
    if (*offset + len + 1 > *capacity) {
        size_t new_capacity = (*capacity + len + 128) * 2;
        char *new_buffer = realloc(*buffer, new_capacity);
        if (!new_buffer) {
            return false;
        }
        *buffer = new_buffer;
        *capacity = new_capacity;
    }
    memcpy(*buffer + *offset, text, len);
    *offset += len;
    (*buffer)[*offset] = '\0';
    return true;
}

static bool append_json(char **buffer, size_t *offset, size_t *capacity, const cJSON *item);

static bool append_json_string(char **buffer, size_t *offset, size_t *capacity, const char *text)
{
    if (!append_text(buffer, offset, capacity, "\"")) {
        return false;
    }
    for (const char *cursor = text ? text : ""; *cursor; ++cursor) {
        char escaped[3] = {'\\', *cursor, '\0'};
        char plain[2] = {*cursor, '\0'};
        if (*cursor == '"' || *cursor == '\\') {
            if (!append_text(buffer, offset, capacity, escaped)) {
                return false;
            }
        } else if (!append_text(buffer, offset, capacity, plain)) {
            return false;
        }
    }
    return append_text(buffer, offset, capacity, "\"");
}

static bool append_pair_value(char **buffer,
                              size_t *offset,
                              size_t *capacity,
                              const host_json_pair_t *pair)
{
    char number_text[48] = {0};

    switch (pair->type) {
    case HOST_JSON_ARRAY:
    case HOST_JSON_OBJECT:
        return append_json(buffer, offset, capacity, pair->child);
    case HOST_JSON_BOOL:
        return append_text(buffer, offset, capacity, pair->boolean ? "true" : "false");
    case HOST_JSON_NUMBER:
        snprintf(number_text, sizeof(number_text), "%.0f", pair->number);
        return append_text(buffer, offset, capacity, number_text);
    case HOST_JSON_STRING:
        return append_json_string(buffer, offset, capacity, pair->string);
    default:
        return false;
    }
}

static bool append_json(char **buffer, size_t *offset, size_t *capacity, const cJSON *item)
{
    if (!item) {
        return append_text(buffer, offset, capacity, "null");
    }

    if (!append_text(buffer, offset, capacity, item->is_array ? "[" : "{")) {
        return false;
    }
    for (size_t i = 0; i < item->count; ++i) {
        const host_json_pair_t *pair = &item->pairs[i];
        if (i > 0 && !append_text(buffer, offset, capacity, ",")) {
            return false;
        }
        if (!item->is_array) {
            if (!append_json_string(buffer, offset, capacity, pair->name) ||
                !append_text(buffer, offset, capacity, ":")) {
                return false;
            }
        }
        if (!append_pair_value(buffer, offset, capacity, pair)) {
            return false;
        }
    }
    return append_text(buffer, offset, capacity, item->is_array ? "]" : "}");
}

char *cJSON_PrintUnformatted(const cJSON *item)
{
    size_t capacity = 512;
    size_t offset = 0;
    char *buffer = calloc(1, capacity);
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
