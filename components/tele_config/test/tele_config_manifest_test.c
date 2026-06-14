#include <assert.h>
#include <string.h>

#include "tele_config.h"

static const tele_config_field_t fields[] = {
    {
        .id = "wifi.provisioning_ssid",
        .nvs_key = "w_pssid",
        .type = TELE_CONFIG_TYPE_STRING,
        .default_value.string = "ESP32-Device",
        .min_len = 1,
        .max_len = 32,
        .flags = TELE_CONFIG_FLAG_WEB | TELE_CONFIG_FLAG_MQTT,
    },
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
        .id = "wifi.internal_only",
        .nvs_key = "w_int",
        .type = TELE_CONFIG_TYPE_BOOL,
        .default_value.boolean = true,
        .flags = TELE_CONFIG_FLAG_WEB,
    },
};

int main(void)
{
    cJSON *root = NULL;
    char *text = NULL;

    assert(tele_config_register_fields(fields, 3) == ESP_OK);

    root = cJSON_CreateObject();
    assert(root != NULL);
    assert(tele_config_add_manifest_to_json(root, TELE_CONFIG_FLAG_MQTT) == ESP_OK);
    text = cJSON_PrintUnformatted(root);
    assert(text != NULL);

    assert(strstr(text, "\"registry_revision\":1") != NULL);
    assert(strstr(text, "\"id\":\"wifi.provisioning_ssid\"") != NULL);
    assert(strstr(text, "\"type\":\"string\"") != NULL);
    assert(strstr(text, "\"default\":\"ESP32-Device\"") != NULL);
    assert(strstr(text, "\"value\":\"ESP32-Device\"") != NULL);
    assert(strstr(text, "\"source\":\"default\"") != NULL);
    assert(strstr(text, "\"min_len\":1") != NULL);
    assert(strstr(text, "\"max_len\":32") != NULL);

    assert(strstr(text, "\"id\":\"wifi.sta_max_retry\"") != NULL);
    assert(strstr(text, "\"type\":\"u32\"") != NULL);
    assert(strstr(text, "\"default\":3") != NULL);
    assert(strstr(text, "\"value\":3") != NULL);
    assert(strstr(text, "\"min\":1") != NULL);
    assert(strstr(text, "\"max\":20") != NULL);
    assert(strstr(text, "\"flag\":\"mqtt\"") != NULL);

    assert(strstr(text, "wifi.internal_only") == NULL);

    cJSON_free(text);
    cJSON_Delete(root);
    return 0;
}
