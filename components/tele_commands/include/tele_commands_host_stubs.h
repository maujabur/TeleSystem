#pragma once

#include <stdbool.h>

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NO_MEM 0x101

typedef struct cJSON cJSON;

cJSON *cJSON_CreateObject(void);
cJSON *cJSON_AddArrayToObject(cJSON *object, const char *name);
bool cJSON_AddItemToArray(cJSON *array, cJSON *item);
bool cJSON_AddBoolToObject(cJSON *object, const char *name, bool value);
double cJSON_AddNumberToObject(cJSON *object, const char *name, double value);
bool cJSON_AddStringToObject(cJSON *object, const char *name, const char *value);
char *cJSON_PrintUnformatted(const cJSON *item);
void cJSON_Delete(cJSON *item);
void cJSON_free(void *object);
