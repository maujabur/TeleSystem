#include <assert.h>

#include "tele_config.h"

static const tele_config_field_t fields[] = {
    {
        .id = "wifi.sta_max_retry",
        .nvs_key = "w_retry",
        .type = TELE_CONFIG_TYPE_U32,
        .default_value.u32 = 3,
        .min.u32 = 1,
        .max.u32 = 20,
        .flags = TELE_CONFIG_FLAG_WEB | TELE_CONFIG_FLAG_MQTT,
    },
};

static const tele_config_field_t more_fields[] = {
    {
        .id = "mqtt.heartbeat_interval_s",
        .nvs_key = "m_hb",
        .type = TELE_CONFIG_TYPE_U32,
        .default_value.u32 = 60,
        .min.u32 = 15,
        .max.u32 = 3600,
        .flags = TELE_CONFIG_FLAG_WEB | TELE_CONFIG_FLAG_MQTT,
    },
};

static const tele_config_field_t invalid_key_field[] = {
    {
        .id = "wifi.invalid_key",
        .nvs_key = "this_key_is_too_long",
        .type = TELE_CONFIG_TYPE_U32,
        .default_value.u32 = 1,
        .min.u32 = 1,
        .max.u32 = 10,
    },
};

static const tele_config_field_t invalid_default_field[] = {
    {
        .id = "wifi.invalid_default",
        .nvs_key = "w_invalid",
        .type = TELE_CONFIG_TYPE_U32,
        .default_value.u32 = 99,
        .min.u32 = 1,
        .max.u32 = 10,
    },
};

int main(void)
{
    assert(tele_config_register_fields(invalid_key_field, 1) == ESP_ERR_INVALID_SIZE);
    assert(tele_config_register_fields(invalid_default_field, 1) == ESP_ERR_INVALID_ARG);
    assert(tele_config_register_fields(fields, 1) == ESP_OK);
    assert(tele_config_register_fields(more_fields, 1) == ESP_OK);
    assert(tele_config_find_field("wifi.sta_max_retry") == &fields[0]);
    assert(tele_config_find_field("mqtt.heartbeat_interval_s") == &more_fields[0]);
    assert(tele_config_find_field("missing") == NULL);
    assert(tele_config_register_fields(fields, 1) == ESP_ERR_INVALID_STATE);
    return 0;
}
