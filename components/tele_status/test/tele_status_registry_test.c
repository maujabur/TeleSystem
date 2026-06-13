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
        .label = "Wi-Fi pronto",
        .description = "Indica se a rede Wi-Fi esta pronta para uso.",
        .group = "network",
        .type = TELE_STATUS_TYPE_BOOL,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_HEARTBEAT | TELE_STATUS_FLAG_MQTT,
        .read.boolean = bool_value,
    },
    {
        .id = "rssi",
        .label = "RSSI",
        .description = "Sinal Wi-Fi recebido.",
        .group = "network",
        .type = TELE_STATUS_TYPE_I32,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_HEARTBEAT | TELE_STATUS_FLAG_MQTT,
        .read.i32 = i32_value,
    },
    {
        .id = "uptime_s",
        .label = "Uptime",
        .description = "Tempo desde o boot em segundos.",
        .group = "runtime",
        .type = TELE_STATUS_TYPE_U32,
        .flags = TELE_STATUS_FLAG_HEARTBEAT | TELE_STATUS_FLAG_TECHNICAL | TELE_STATUS_FLAG_MQTT,
        .read.u32 = u32_value,
        .ctx = &(uint32_t) {123},
    },
    {
        .id = "ip",
        .label = "IP",
        .description = "Endereco IPv4 atual.",
        .group = "network",
        .type = TELE_STATUS_TYPE_STRING,
        .flags = TELE_STATUS_FLAG_STATE | TELE_STATUS_FLAG_MQTT,
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

    root = cJSON_CreateObject();
    assert(root != NULL);
    assert(tele_status_add_manifest_to_json(root, TELE_STATUS_FLAG_MQTT) == ESP_OK);
    text = cJSON_PrintUnformatted(root);
    assert(text != NULL);
    assert(strstr(text, "\"registry_revision\":1") != NULL);
    assert(strstr(text, "\"fields\"") != NULL);
    assert(strstr(text, "\"id\":\"wifi_ready\"") != NULL);
    assert(strstr(text, "\"label\":\"Wi-Fi pronto\"") != NULL);
    assert(strstr(text, "\"description\":\"Indica se a rede Wi-Fi esta pronta para uso.\"") != NULL);
    assert(strstr(text, "\"group\":\"network\"") != NULL);
    assert(strstr(text, "\"type\":\"bool\"") != NULL);
    assert(strstr(text, "\"flag\":\"state\"") != NULL);
    assert(strstr(text, "\"id\":\"rssi\"") != NULL);
    assert(strstr(text, "\"type\":\"i32\"") != NULL);
    assert(strstr(text, "\"id\":\"uptime_s\"") != NULL);
    assert(strstr(text, "\"group\":\"runtime\"") != NULL);
    assert(strstr(text, "\"type\":\"u32\"") != NULL);
    assert(strstr(text, "\"flag\":\"technical\"") != NULL);
    assert(strstr(text, "\"id\":\"ip\"") != NULL);
    assert(strstr(text, "\"type\":\"string\"") != NULL);
    cJSON_free(text);
    cJSON_Delete(root);

    return 0;
}
