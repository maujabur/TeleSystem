#include <assert.h>

#include "tele_config.h"

static void test_u32_range_validation(void)
{
    tele_config_field_t field = {
        .id = "wifi.sta_max_retry",
        .nvs_key = "w_retry",
        .type = TELE_CONFIG_TYPE_U32,
        .default_value.u32 = 3,
        .min.u32 = 1,
        .max.u32 = 20,
    };
    tele_config_value_t value = {.u32 = 0};

    assert(tele_config_validate_value(&field, &value) == ESP_ERR_INVALID_ARG);
    value.u32 = 1;
    assert(tele_config_validate_value(&field, &value) == ESP_OK);
    value.u32 = 20;
    assert(tele_config_validate_value(&field, &value) == ESP_OK);
    value.u32 = 21;
    assert(tele_config_validate_value(&field, &value) == ESP_ERR_INVALID_ARG);
}

static void test_string_length_validation(void)
{
    tele_config_field_t field = {
        .id = "wifi.provisioning_ssid",
        .nvs_key = "w_pssid",
        .type = TELE_CONFIG_TYPE_STRING,
        .default_value.string = "TeleCafezinho",
        .min_len = 1,
        .max_len = 32,
    };
    tele_config_value_t value = {.string = ""};

    assert(tele_config_validate_value(&field, &value) == ESP_ERR_INVALID_ARG);
    value.string = "TeleCafezinho";
    assert(tele_config_validate_value(&field, &value) == ESP_OK);
    value.string = "123456789012345678901234567890123";
    assert(tele_config_validate_value(&field, &value) == ESP_ERR_INVALID_SIZE);
}

int main(void)
{
    test_u32_range_validation();
    test_string_length_validation();
    return 0;
}
