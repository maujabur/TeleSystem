#include <assert.h>
#include <stdbool.h>

#include "tele_config.h"

static int callback_calls;

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
    {
        .id = "wifi.apsta_policy",
        .nvs_key = "w_apsta",
        .type = TELE_CONFIG_TYPE_ENUM,
        .default_value.i32 = 1,
        .min.i32 = 0,
        .max.i32 = 2,
        .flags = TELE_CONFIG_FLAG_WEB | TELE_CONFIG_FLAG_MQTT | TELE_CONFIG_FLAG_REBOOT_REQUIRED,
    },
};

static esp_err_t apply_callback(const tele_config_field_t *field,
                                const tele_config_value_t *value,
                                void *ctx)
{
    (void)field;
    (void)value;
    (void)ctx;
    callback_calls++;
    return ESP_OK;
}

int main(void)
{
    tele_config_update_result_t result = {0};
    tele_config_value_t retry = {.u32 = 5};
    tele_config_value_t invalid_retry = {.u32 = 30};
    tele_config_value_t policy = {.i32 = 2};

    assert(tele_config_register_fields(fields, 2) == ESP_OK);
    assert(tele_config_set_apply_handler("wifi.sta_max_retry", apply_callback, NULL) == ESP_OK);

    assert(tele_config_update_value("wifi.sta_max_retry", &retry, &result) == ESP_OK);
    assert(callback_calls == 1);
    assert(result.applied == true);
    assert(result.stored == true);
    assert(result.requires_reboot == false);

    result = (tele_config_update_result_t){0};
    assert(tele_config_update_value("wifi.sta_max_retry", &invalid_retry, &result) == ESP_ERR_INVALID_ARG);
    assert(callback_calls == 1);
    assert(result.applied == false);
    assert(result.stored == false);

    result = (tele_config_update_result_t){0};
    assert(tele_config_update_value("wifi.apsta_policy", &policy, &result) == ESP_OK);
    assert(callback_calls == 1);
    assert(result.applied == false);
    assert(result.stored == true);
    assert(result.requires_reboot == true);

    assert(tele_config_update_value("missing.field", &policy, NULL) == ESP_ERR_INVALID_ARG);
    return 0;
}
