#include "tele_portal_config.h"

#include <stdint.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "http_helpers.h"
#include "tele_config.h"
#include "tele_portal_core.h"

#ifndef CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS
#define CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS 0
#endif

static const char *TAG = "portal-config";

static esp_err_t send_json_error(httpd_req_t *req, int status_code, const char *error)
{
    cJSON *json = cJSON_CreateObject();
    esp_err_t err = ESP_OK;

    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(json, "ok", false);
    cJSON_AddStringToObject(json, "error", error ? error : "unknown_error");
    err = http_helpers_send_json(req, json, status_code);
    cJSON_Delete(json);
    return err;
}

static esp_err_t json_to_config_value(const tele_config_field_t *field,
                                      const cJSON *json_value,
                                      tele_config_value_t *out_value,
                                      const char **out_error)
{
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

static cJSON *build_config_update_result(const char *id, const tele_config_update_result_t *update)
{
    cJSON *result = cJSON_CreateObject();
    if (!result) {
        return NULL;
    }

    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddStringToObject(result, "id", id ? id : "");
    if (update) {
        cJSON_AddBoolToObject(result, "stored", update->stored);
        cJSON_AddBoolToObject(result, "applied", update->applied);
        cJSON_AddBoolToObject(result, "requires_reboot", update->requires_reboot);
    }
    return result;
}

static esp_err_t api_config_meta_get_handler(httpd_req_t *req)
{
    cJSON *json = cJSON_CreateObject();
    esp_err_t err = ESP_OK;

    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    tele_portal_core_note_activity();
    err = tele_config_add_manifest_to_json(json, TELE_CHANNEL_FLAG_WEB);
    if (err == ESP_OK) {
        err = http_helpers_send_json(req, json, 200);
    }
    cJSON_Delete(json);
    return err;
}

static esp_err_t api_config_set_post_handler(httpd_req_t *req)
{
    char body[384];
    cJSON *json = NULL;
    cJSON *result = NULL;
    const cJSON *id_item = NULL;
    const cJSON *value_item = NULL;
    const tele_config_field_t *field = NULL;
    tele_config_value_t value = {0};
    tele_config_update_result_t update = {0};
    const char *parse_error = NULL;
    esp_err_t err = http_helpers_recv_body(req, body, sizeof(body));

    tele_portal_core_note_activity();
    if (err != ESP_OK) {
        return send_json_error(req, 400, "body_invalid");
    }

    json = cJSON_Parse(body);
    if (!json) {
        return send_json_error(req, 400, "json_invalid");
    }

    id_item = cJSON_GetObjectItemCaseSensitive(json, "id");
    value_item = cJSON_GetObjectItemCaseSensitive(json, "value");
    if (!cJSON_IsString(id_item) || !id_item->valuestring || id_item->valuestring[0] == '\0') {
        cJSON_Delete(json);
        return send_json_error(req, 400, "missing_config_id");
    }

    field = tele_config_find_field(id_item->valuestring);
    if (!field || (field->channel_flags & TELE_CHANNEL_FLAG_WEB) == 0) {
        cJSON_Delete(json);
        return send_json_error(req, 404, "config_field_not_found");
    }

    err = json_to_config_value(field, value_item, &value, &parse_error);
    if (err != ESP_OK) {
        cJSON_Delete(json);
        return send_json_error(req, 400, parse_error ? parse_error : "config_value_invalid");
    }

    err = tele_config_update_value(field->id, &value, &update);
    result = build_config_update_result(field->id, &update);
    cJSON_Delete(json);
    if (!result) {
        return ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        cJSON_Delete(result);
        return send_json_error(req,
                               err == ESP_ERR_INVALID_STATE ? 409 : 400,
                               CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS ? esp_err_to_name(err) :
                               "config_update_failed");
    }

    err = http_helpers_send_json(req, result, 200);
    cJSON_Delete(result);
    return err;
}

static esp_err_t api_config_reset_post_handler(httpd_req_t *req)
{
    char body[192];
    cJSON *json = NULL;
    cJSON *result = NULL;
    const cJSON *id_item = NULL;
    const tele_config_field_t *field = NULL;
    tele_config_update_result_t update = {0};
    esp_err_t err = http_helpers_recv_body(req, body, sizeof(body));

    tele_portal_core_note_activity();
    if (err != ESP_OK) {
        return send_json_error(req, 400, "body_invalid");
    }

    json = cJSON_Parse(body);
    if (!json) {
        return send_json_error(req, 400, "json_invalid");
    }

    id_item = cJSON_GetObjectItemCaseSensitive(json, "id");
    if (!cJSON_IsString(id_item) || !id_item->valuestring || id_item->valuestring[0] == '\0') {
        cJSON_Delete(json);
        return send_json_error(req, 400, "missing_config_id");
    }

    field = tele_config_find_field(id_item->valuestring);
    if (!field || (field->channel_flags & TELE_CHANNEL_FLAG_WEB) == 0) {
        cJSON_Delete(json);
        return send_json_error(req, 404, "config_field_not_found");
    }

    err = tele_config_reset_value(field->id, &update);
    result = build_config_update_result(field->id, &update);
    cJSON_Delete(json);
    if (!result) {
        return ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        cJSON_Delete(result);
        return send_json_error(req,
                               err == ESP_ERR_INVALID_STATE ? 409 : 400,
                               CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS ? esp_err_to_name(err) :
                               "config_reset_failed");
    }

    err = http_helpers_send_json(req, result, 200);
    cJSON_Delete(result);
    return err;
}

static esp_err_t api_config_apply_reboot_post_handler(httpd_req_t *req)
{
    cJSON *json = cJSON_CreateObject();

    tele_portal_core_note_activity();
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(json, "ok", true);
    cJSON_AddNumberToObject(json, "delay_ms", 800);
    esp_err_t err = http_helpers_send_json(req, json, 200);
    cJSON_Delete(json);
    tele_portal_core_request_restart(800);
    return err;
}

static esp_err_t register_uri_checked(httpd_handle_t server, const httpd_uri_t *route)
{
    esp_err_t err = httpd_register_uri_handler(server, route);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar rota %s: %s", route->uri, esp_err_to_name(err));
    }
    return err;
}

esp_err_t tele_portal_config_register_routes(httpd_handle_t server)
{
    httpd_uri_t api_meta = {
        .uri = "/api/config/meta",
        .method = HTTP_GET,
        .handler = api_config_meta_get_handler,
    };
    httpd_uri_t api_set = {
        .uri = "/api/config/set",
        .method = HTTP_POST,
        .handler = api_config_set_post_handler,
    };
    httpd_uri_t api_reset = {
        .uri = "/api/config/reset",
        .method = HTTP_POST,
        .handler = api_config_reset_post_handler,
    };
    httpd_uri_t api_apply_reboot = {
        .uri = "/api/config/apply-reboot",
        .method = HTTP_POST,
        .handler = api_config_apply_reboot_post_handler,
    };

    esp_err_t err = register_uri_checked(server, &api_meta);
    if (err == ESP_OK) {
        err = register_uri_checked(server, &api_set);
    }
    if (err == ESP_OK) {
        err = register_uri_checked(server, &api_reset);
    }
    if (err == ESP_OK) {
        err = register_uri_checked(server, &api_apply_reboot);
    }
    return err;
}
