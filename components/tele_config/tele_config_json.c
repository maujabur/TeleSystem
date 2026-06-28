#include "tele_config.h"

#include <stdint.h>

esp_err_t tele_config_value_from_json(const tele_config_field_t *field,
                                      const cJSON *json_value,
                                      tele_config_value_t *out_value,
                                      const char **out_error)
{
    if (out_error) {
        *out_error = NULL;
    }
    if (!field || !json_value || !out_value) {
        if (out_error) {
            *out_error = "missing_config_value";
        }
        return ESP_ERR_INVALID_ARG;
    }

    switch (field->type) {
    case TELE_CONFIG_TYPE_BOOL:
        if (!cJSON_IsBool(json_value)) {
            if (out_error) {
                *out_error = "config_value_type_bool_required";
            }
            return ESP_ERR_INVALID_ARG;
        }
        out_value->boolean = cJSON_IsTrue(json_value);
        return ESP_OK;
    case TELE_CONFIG_TYPE_I32:
    case TELE_CONFIG_TYPE_ENUM:
        if (!cJSON_IsNumber(json_value)) {
            if (out_error) {
                *out_error = "config_value_type_i32_required";
            }
            return ESP_ERR_INVALID_ARG;
        }
        out_value->i32 = (int32_t)json_value->valuedouble;
        return ESP_OK;
    case TELE_CONFIG_TYPE_U32:
        if (!cJSON_IsNumber(json_value) || json_value->valuedouble < 0) {
            if (out_error) {
                *out_error = "config_value_type_u32_required";
            }
            return ESP_ERR_INVALID_ARG;
        }
        out_value->u32 = (uint32_t)json_value->valuedouble;
        return ESP_OK;
    case TELE_CONFIG_TYPE_STRING:
        if (!cJSON_IsString(json_value) || !json_value->valuestring) {
            if (out_error) {
                *out_error = "config_value_type_string_required";
            }
            return ESP_ERR_INVALID_ARG;
        }
        out_value->string = json_value->valuestring;
        return ESP_OK;
    default:
        if (out_error) {
            *out_error = "config_field_type_unsupported";
        }
        return ESP_ERR_INVALID_ARG;
    }
}
