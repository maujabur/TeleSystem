#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tele_status.h"

static bool bool_value(void *ctx)
{
    (void)ctx;
    return true;
}

static int32_t i32_value(void *ctx)
{
    (void)ctx;
    return -42;
}

static uint32_t u32_value(void *ctx)
{
    return *(uint32_t *)ctx;
}

static const char *string_value(void *ctx)
{
    return (const char *)ctx;
}

static const tele_status_field_t fields[] = {
    {
        .id = "wifi_ready",
        .type = TELE_STATUS_TYPE_BOOL,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_HEARTBEAT,
        .read.boolean = bool_value,
    },
    {
        .id = "rssi",
        .type = TELE_STATUS_TYPE_I32,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_HEARTBEAT,
        .read.i32 = i32_value,
    },
    {
        .id = "uptime_s",
        .type = TELE_STATUS_TYPE_U32,
        .flags = TELE_STATUS_FLAG_HEARTBEAT,
        .read.u32 = u32_value,
        .ctx = &(uint32_t) {123},
    },
    {
        .id = "ip",
        .type = TELE_STATUS_TYPE_STRING,
        .flags = TELE_STATUS_FLAG_STATE,
        .read.string = string_value,
        .ctx = "192.168.15.97",
    },
};

int main(void)
{
    cJSON *root = NULL;
    char *text = NULL;

    assert(tele_status_register_fields(fields, 4) == ESP_OK);
    assert(tele_status_find_field("rssi") == &fields[1]);
    assert(tele_status_find_field("missing") == NULL);
    assert(tele_status_register_fields(fields, 1) == ESP_ERR_INVALID_STATE);

    root = cJSON_CreateObject();
    assert(root != NULL);
    assert(tele_status_add_fields_to_json(root, TELE_STATUS_FLAG_HEARTBEAT) == ESP_OK);
    text = cJSON_PrintUnformatted(root);
    assert(text != NULL);
    assert(strstr(text, "\"wifi_ready\":true") != NULL);
    assert(strstr(text, "\"rssi\":-42") != NULL);
    assert(strstr(text, "\"uptime_s\":123") != NULL);
    assert(strstr(text, "\"ip\"") == NULL);
    cJSON_free(text);
    cJSON_Delete(root);

    root = cJSON_CreateObject();
    assert(root != NULL);
    assert(tele_status_add_fields_to_json(root, TELE_STATUS_FLAG_STATE) == ESP_OK);
    text = cJSON_PrintUnformatted(root);
    assert(text != NULL);
    assert(strstr(text, "\"wifi_ready\":true") != NULL);
    assert(strstr(text, "\"rssi\":-42") != NULL);
    assert(strstr(text, "\"ip\":\"192.168.15.97\"") != NULL);
    assert(strstr(text, "\"uptime_s\"") == NULL);
    cJSON_free(text);
    cJSON_Delete(root);

    return 0;
}
