#include "tele_config.h"

#include <string.h>

#ifndef TELE_CONFIG_HOST_TEST
#include "nvs.h"
#endif

#define TELE_CONFIG_MAX_REGISTERED_FIELDS 64
#define TELE_CONFIG_NVS_NAMESPACE "tele_config"

static const tele_config_field_t *s_fields[TELE_CONFIG_MAX_REGISTERED_FIELDS];
static size_t s_field_count;

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
        s_fields[s_field_count++] = &fields[i];
    }
    return ESP_OK;
}

const tele_config_field_t *tele_config_find_field(const char *id)
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

#ifdef TELE_CONFIG_HOST_TEST
    return ESP_ERR_NOT_SUPPORTED;
#else
    char key[TELE_CONFIG_NVS_KEY_MAX_LEN + 1] = {0};
    err = copy_nvs_key(field, key, sizeof(key));
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

esp_err_t tele_config_reset_override(const char *id)
{
    const tele_config_field_t *field = tele_config_find_field(id);
    if (!field) {
        return ESP_ERR_INVALID_ARG;
    }

#ifdef TELE_CONFIG_HOST_TEST
    return ESP_ERR_NOT_SUPPORTED;
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
