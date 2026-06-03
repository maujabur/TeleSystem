#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "acr_parser.h"

static const char *TAG = "acr-parser";

static cJSON *parse_json_root(const char *json)
{
    if (!json || json[0] == '\0') {
        return NULL;
    }

    return cJSON_Parse(json);
}

static int extract_file_item_result(const cJSON *item,
                                    const char *expected_name,
                                    acr_result_t *result)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(item, "state");
    const cJSON *results = NULL;
    const cJSON *ai_detection = NULL;
    const cJSON *detection = NULL;
    const cJSON *prediction = NULL;
    const cJSON *ai_probability = NULL;

    if (!cJSON_IsString(name) || !name->valuestring || strcmp(name->valuestring, expected_name) != 0) {
        return 0;
    }

    if (!cJSON_IsNumber(state)) {
        return 0;
    }

    result->found = 1;
    result->state = state->valueint;

    results = cJSON_GetObjectItemCaseSensitive(item, "results");
    ai_detection = cJSON_GetObjectItemCaseSensitive(results, "ai_detection");
    detection = cJSON_IsArray(ai_detection) ? cJSON_GetArrayItem(ai_detection, 0) : NULL;

    if (cJSON_IsObject(detection)) {
        prediction = cJSON_GetObjectItemCaseSensitive(detection, "prediction");
        ai_probability = cJSON_GetObjectItemCaseSensitive(detection, "ai_probability");

        if (cJSON_IsString(prediction) && prediction->valuestring) {
            snprintf(result->prediction, sizeof(result->prediction), "%s", prediction->valuestring);
        }
        if (cJSON_IsNumber(ai_probability)) {
            result->ai_probability = ai_probability->valuedouble;
        }
    }

    return 1;
}

int acr_parser_extract_matching_file_result(const char *json_response,
                                            const char *expected_name,
                                            acr_result_t *result)
{
    cJSON *root = NULL;
    cJSON *data = NULL;
    cJSON *item = NULL;

    if (!json_response || !expected_name || !result) {
        return 0;
    }

    memset(result, 0, sizeof(*result));
    root = parse_json_root(json_response);
    if (!root) {
        return 0;
    }

    data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsObject(data)) {
        int found = extract_file_item_result(data, expected_name, result);
        cJSON_Delete(root);
        return found;
    }

    if (!cJSON_IsArray(data)) {
        cJSON_Delete(root);
        return 0;
    }

    cJSON_ArrayForEach(item, data) {
        if (extract_file_item_result(item, expected_name, result)) {
            cJSON_Delete(root);
            return 1;
        }
    }

    cJSON_Delete(root);
    return 0;
}

void acr_parser_log_result_summary(const acr_result_t *result)
{
    if (!result || !result->found) {
        ESP_LOGW(TAG, "Resultado da ACR indisponivel");
        return;
    }

    ESP_LOGI(TAG,
             "Resultado ACR bruto: prediction=%s | Probabilidade: %.2f%%",
             result->prediction[0] != '\0' ? result->prediction : "(vazio)",
             result->ai_probability);
}
