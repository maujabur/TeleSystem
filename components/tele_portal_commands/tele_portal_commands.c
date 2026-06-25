#include "tele_portal_commands.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "http_helpers.h"
#include "tele_commands.h"
#include "tele_portal_core.h"

#ifndef CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS
#define CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS 0
#endif

#define TELE_PORTAL_COMMAND_BODY_SIZE 1024

static const char *TAG = "portal-commands";

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

static esp_err_t api_commands_meta_get_handler(httpd_req_t *req)
{
    cJSON *json = cJSON_CreateObject();
    esp_err_t err = ESP_OK;

    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    tele_portal_core_note_activity();
    err = tele_commands_add_manifest_to_json(json, TELE_COMMAND_FLAG_WEB);
    if (err == ESP_OK) {
        err = http_helpers_send_json(req, json, 200);
    }
    cJSON_Delete(json);
    return err;
}

static cJSON *build_command_http_response(const char *cmd_id,
                                          const tele_command_response_t *response)
{
    cJSON *json = cJSON_CreateObject();

    if (!json || !response) {
        cJSON_Delete(json);
        return NULL;
    }

    cJSON_AddStringToObject(json, "cmd_id", cmd_id ? cmd_id : "unknown");
    cJSON_AddBoolToObject(json, "ok", response->ok);
    if (response->error && response->error[0] != '\0') {
        cJSON_AddStringToObject(json, "error", response->error);
    }
    if (response->result) {
        cJSON *result = cJSON_Duplicate(response->result, 1);
        if (result) {
            cJSON_AddItemToObject(json, "result", result);
        }
    }
    return json;
}

static esp_err_t api_commands_execute_post_handler(httpd_req_t *req)
{
    char body[TELE_PORTAL_COMMAND_BODY_SIZE];
    cJSON *json = NULL;
    cJSON *response_json = NULL;
    cJSON *cmd_id_item = NULL;
    cJSON *name_item = NULL;
    cJSON *args = NULL;
    const char *cmd_id = "unknown";
    const char *cmd_name = NULL;
    tele_command_response_t response = {0};
    esp_err_t err = http_helpers_recv_body(req, body, sizeof(body));

    tele_portal_core_note_activity();
    if (err != ESP_OK) {
        return send_json_error(req, 400, "body_invalid");
    }

    json = cJSON_Parse(body);
    if (!json) {
        return send_json_error(req, 400, "json_invalid");
    }

    cmd_id_item = cJSON_GetObjectItemCaseSensitive(json, "cmd_id");
    if (cJSON_IsString(cmd_id_item) && cmd_id_item->valuestring) {
        cmd_id = cmd_id_item->valuestring;
    }

    name_item = cJSON_GetObjectItemCaseSensitive(json, "name");
    if (!cJSON_IsString(name_item) || !name_item->valuestring ||
        name_item->valuestring[0] == '\0') {
        cJSON_Delete(json);
        return send_json_error(req, 400, "missing_name");
    }
    cmd_name = name_item->valuestring;
    args = cJSON_GetObjectItemCaseSensitive(json, "args");

    const tele_command_request_t command_request = {
        .cmd_id = cmd_id,
        .name = cmd_name,
        .args = args,
        .required_flags = TELE_COMMAND_FLAG_WEB,
    };
    err = tele_commands_execute(&command_request, &response);
    if (err != ESP_OK) {
        cJSON_Delete(json);
        return send_json_error(req,
                               500,
                               CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS ? esp_err_to_name(err) :
                               "command_dispatch_failed");
    }

    response_json = build_command_http_response(cmd_id, &response);
    tele_commands_response_cleanup(&response);
    cJSON_Delete(json);
    if (!response_json) {
        return ESP_ERR_NO_MEM;
    }

    err = http_helpers_send_json(req, response_json, response.ok ? 200 : 400);
    cJSON_Delete(response_json);
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

esp_err_t tele_portal_commands_register_routes(httpd_handle_t server)
{
    httpd_uri_t api_commands_meta = {
        .uri = "/api/commands",
        .method = HTTP_GET,
        .handler = api_commands_meta_get_handler,
    };
    httpd_uri_t api_commands_execute = {
        .uri = "/api/commands/execute",
        .method = HTTP_POST,
        .handler = api_commands_execute_post_handler,
    };

    esp_err_t err = register_uri_checked(server, &api_commands_meta);
    if (err == ESP_OK) {
        err = register_uri_checked(server, &api_commands_execute);
    }
    return err;
}
