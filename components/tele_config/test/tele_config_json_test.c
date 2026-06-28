#include <assert.h>
#include <string.h>

#include "cJSON.h"
#include "tele_config.h"

int main(void)
{
    const char *error = NULL;
    tele_config_value_t value = {0};
    tele_config_field_t bool_field = {
        .id = "feature.enabled",
        .type = TELE_CONFIG_TYPE_BOOL,
    };
    tele_config_field_t u32_field = {
        .id = "mqtt.heartbeat_interval_s",
        .type = TELE_CONFIG_TYPE_U32,
        .min.u32 = 15,
        .max.u32 = 3600,
    };
    tele_config_field_t string_field = {
        .id = "wifi.ssid",
        .type = TELE_CONFIG_TYPE_STRING,
        .min_len = 1,
        .max_len = 32,
    };

    cJSON *json_bool = cJSON_CreateBool(1);
    assert(json_bool != NULL);
    assert(tele_config_value_from_json(&bool_field, json_bool, &value, &error) == ESP_OK);
    assert(value.boolean);
    assert(error == NULL);
    cJSON_Delete(json_bool);

    cJSON *json_u32 = cJSON_CreateNumber(60);
    assert(json_u32 != NULL);
    assert(tele_config_value_from_json(&u32_field, json_u32, &value, &error) == ESP_OK);
    assert(value.u32 == 60);
    assert(error == NULL);
    cJSON_Delete(json_u32);

    cJSON *json_string = cJSON_CreateString("TeleCafezinho");
    assert(json_string != NULL);
    assert(tele_config_value_from_json(&string_field, json_string, &value, &error) == ESP_OK);
    assert(value.string != NULL);
    assert(strcmp(value.string, "TeleCafezinho") == 0);
    assert(error == NULL);
    cJSON_Delete(json_string);

    cJSON *bad_u32 = cJSON_CreateString("60");
    assert(bad_u32 != NULL);
    assert(tele_config_value_from_json(&u32_field, bad_u32, &value, &error) == ESP_ERR_INVALID_ARG);
    assert(error && strcmp(error, "config_value_type_u32_required") == 0);
    cJSON_Delete(bad_u32);

    return 0;
}
