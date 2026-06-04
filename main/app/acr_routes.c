#include "cJSON.h"
#include "esp_log.h"
#include "esp_http_server.h"

#include "acr_analysis_control.h"
#include "acr_config_store.h"
#include "acr_orchestrator.h"
#include "acr_runtime_status.h"
#include "acr_routes.h"
#include "acr_trigger_output.h"
#include "http_helpers.h"
#include "web_portal.h"

#ifndef CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS
#define CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS 0
#endif

static const char *http_error_message(esp_err_t err)
{
    if (CONFIG_WEB_PORTAL_DETAILED_HTTP_ERRORS) {
        return esp_err_to_name(err);
    }
    return "Falha interna ao processar solicitacao";
}

static esp_err_t api_config_get_handler(httpd_req_t *req)
{
    acr_config_public_info_t config;
    cJSON *json = cJSON_CreateObject();
    esp_err_t err = acr_config_store_get_public_info(&config);

    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    if (err == ESP_OK) {
        cJSON_AddStringToObject(json, "region", config.region);
        cJSON_AddStringToObject(json, "container_id", config.container_id);
        cJSON_AddStringToObject(json, "upload_prefix", config.upload_prefix);
        cJSON_AddBoolToObject(json, "token_configured", config.token_configured);
    } else {
        cJSON_AddStringToObject(json, "region", "");
        cJSON_AddStringToObject(json, "container_id", "");
        cJSON_AddStringToObject(json, "upload_prefix", "");
        cJSON_AddBoolToObject(json, "token_configured", false);
    }

    err = http_helpers_send_json(req, json, 200);
    cJSON_Delete(json);
    return err;
}

static esp_err_t api_acr_status_get_handler(httpd_req_t *req)
{
    acr_runtime_status_t status = {0};
    acr_trigger_output_status_t trigger_status = {0};
    cJSON *json = cJSON_CreateObject();

    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    acr_runtime_status_get(&status);
    (void)acr_trigger_output_get_status(&trigger_status);

    cJSON_AddBoolToObject(json, "acr_retry_pending", status.retry_pending);
    cJSON_AddNumberToObject(json, "acr_retry_at_ms", (double)status.retry_at_ms);
    int64_t retry_remaining_ms = status.retry_at_ms - (int64_t)esp_log_timestamp();
    cJSON_AddNumberToObject(json, "acr_retry_remaining_ms",
                            status.retry_pending && retry_remaining_ms > 0 ?
                            (double)retry_remaining_ms : 0);
    cJSON_AddStringToObject(json, "acr_last_error",
                            status.last_error == ESP_OK ? "" : esp_err_to_name(status.last_error));
    cJSON_AddNumberToObject(json, "acr_consecutive_errors", status.consecutive_errors);
    cJSON_AddNumberToObject(json, "acr_submitted_count", status.acr_submitted_count);
    cJSON_AddNumberToObject(json, "acr_silence_discarded_count", status.silence_discarded_count);
    cJSON_AddNumberToObject(json, "acr_error_count", status.acr_error_count);
    cJSON_AddStringToObject(json, "acr_status", status.message);
    cJSON_AddStringToObject(json, "acr_state", acr_runtime_status_state_name(status.state));
    cJSON_AddStringToObject(json, "acr_uploaded_name", status.uploaded_name);
    cJSON_AddNumberToObject(json, "audio_last_capture_size", (double)status.audio_last_capture_size);
    cJSON_AddNumberToObject(json, "audio_last_active_ms", status.audio_last_active_ms);
    cJSON_AddNumberToObject(json, "audio_last_rms", status.audio_last_rms);
    cJSON_AddNumberToObject(json, "audio_last_peak_percent", status.audio_last_peak_percent);
    cJSON_AddBoolToObject(json, "audio_last_clipped", status.audio_last_clipped);
    cJSON_AddBoolToObject(json, "audio_last_clipped_detected", status.audio_last_clipped_detected);
    cJSON_AddBoolToObject(json, "audio_last_silence_discarded", status.audio_last_silence_discarded);
    cJSON_AddBoolToObject(json, "audio_last_silence_detected", status.audio_last_silence_detected);
    cJSON_AddBoolToObject(json, "acr_last_trigger", status.last_trigger);
    cJSON_AddBoolToObject(json, "bt_next_enabled", trigger_status.config.enabled);
    cJSON_AddNumberToObject(json, "bt_next_gpio", trigger_status.config.gpio);
    cJSON_AddNumberToObject(json, "bt_next_active_level", trigger_status.config.active_level);
    cJSON_AddNumberToObject(json, "bt_next_pulse_ms", trigger_status.config.pulse_ms);
    cJSON_AddNumberToObject(json, "bt_next_last_pulse_at_ms", (double)trigger_status.last_pulse_at_ms);
    cJSON_AddStringToObject(json, "bt_next_last_error",
                            trigger_status.last_error == ESP_OK ? "" :
                            esp_err_to_name(trigger_status.last_error));
    cJSON_AddNumberToObject(json, "acr_last_ai_probability", status.last_ai_probability);
    cJSON_AddStringToObject(json, "acr_last_prediction", status.last_prediction);
    cJSON_AddStringToObject(json, "acr_last_uploaded_name", status.last_uploaded_name);
    cJSON_AddNumberToObject(json, "acr_last_result_at_ms", (double)status.last_result_at_ms);
    cJSON_AddNumberToObject(json, "acr_capture_ms", (double)status.capture_ms);
    cJSON_AddNumberToObject(json, "acr_upload_ms", (double)status.upload_ms);
    cJSON_AddNumberToObject(json, "acr_upload_connect_ms", (double)status.upload_connect_ms);
    cJSON_AddNumberToObject(json, "acr_upload_write_ms", (double)status.upload_write_ms);
    cJSON_AddNumberToObject(json, "acr_upload_response_ms", (double)status.upload_response_ms);
    cJSON_AddNumberToObject(json, "acr_response_wait_ms", (double)status.response_wait_ms);
    cJSON_AddNumberToObject(json, "acr_total_cycle_ms", (double)status.total_cycle_ms);

    esp_err_t err = http_helpers_send_json(req, json, 200);
    cJSON_Delete(json);
    return err;
}

static esp_err_t api_acr_post_handler(httpd_req_t *req)
{
    char body[1600];
    cJSON *json = NULL;
    const cJSON *region = NULL;
    const cJSON *container_id = NULL;
    const cJSON *upload_prefix = NULL;
    const cJSON *bearer_token = NULL;
    esp_err_t err = http_helpers_recv_body(req, body, sizeof(body));

    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Body invalido");
    }

    json = cJSON_Parse(body);
    if (!json) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "JSON invalido");
    }

    region = cJSON_GetObjectItemCaseSensitive(json, "region");
    container_id = cJSON_GetObjectItemCaseSensitive(json, "container_id");
    upload_prefix = cJSON_GetObjectItemCaseSensitive(json, "upload_prefix");
    bearer_token = cJSON_GetObjectItemCaseSensitive(json, "bearer_token");

    if (cJSON_IsString(region) && region->valuestring && region->valuestring[0] != '\0') {
        err = acr_config_store_save_region(region->valuestring);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }

    if (cJSON_IsString(container_id) && container_id->valuestring && container_id->valuestring[0] != '\0') {
        err = acr_config_store_save_container_id(container_id->valuestring);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }

    if (cJSON_IsString(upload_prefix) && upload_prefix->valuestring && upload_prefix->valuestring[0] != '\0') {
        err = acr_config_store_save_upload_prefix(upload_prefix->valuestring);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }

    if (cJSON_IsString(bearer_token) && bearer_token->valuestring && bearer_token->valuestring[0] != '\0') {
        err = acr_config_store_save_bearer_token(bearer_token->valuestring);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }

cleanup:
    cJSON_Delete(json);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, http_error_message(err));
    }

    return httpd_resp_sendstr(req, "Configuracao ACR salva.");
}

static esp_err_t api_acr_run_post_handler(httpd_req_t *req)
{
    esp_err_t err = acr_orchestrator_force_cycle();

    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "force_cycle recusado: orquestrador ocupado.");
    }
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, http_error_message(err));
    }

    return httpd_resp_sendstr(req, "force_cycle solicitado.");
}

static esp_err_t api_acr_control_get_handler(httpd_req_t *req)
{
    acr_analysis_control_config_t config = {0};
    cJSON *json = cJSON_CreateObject();
    esp_err_t err = acr_analysis_control_get_config(&config);

    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    if (err == ESP_OK) {
        cJSON_AddBoolToObject(json, "automatic_enabled", config.automatic_enabled);
        cJSON_AddNumberToObject(json, "automatic_interval_ms", config.automatic_interval_ms);
        cJSON_AddNumberToObject(json, "capture_duration_seconds", config.capture_duration_seconds);
        cJSON_AddNumberToObject(json, "digital_gain", config.digital_gain);
        cJSON_AddNumberToObject(json, "silence_threshold_rms", config.silence_threshold_rms);
        cJSON_AddNumberToObject(json, "silence_hysteresis_rms", config.silence_hysteresis_rms);
        cJSON_AddNumberToObject(json, "min_active_ms", config.min_active_ms);
        cJSON_AddNumberToObject(json, "trigger_mode", config.trigger_mode);
        cJSON_AddNumberToObject(json, "ai_probability_threshold", config.ai_probability_threshold);
    } else {
        cJSON_AddStringToObject(json, "error", http_error_message(err));
    }

    err = http_helpers_send_json(req, json, err == ESP_OK ? 200 : 500);
    cJSON_Delete(json);
    return err;
}

static esp_err_t api_acr_control_post_handler(httpd_req_t *req)
{
    char body[384];
    cJSON *json = NULL;
    const cJSON *automatic_enabled = NULL;
    const cJSON *automatic_interval_ms = NULL;
    const cJSON *capture_duration_seconds = NULL;
    const cJSON *digital_gain = NULL;
    const cJSON *silence_threshold_rms = NULL;
    const cJSON *silence_hysteresis_rms = NULL;
    const cJSON *min_active_ms = NULL;
    const cJSON *trigger_mode = NULL;
    const cJSON *ai_probability_threshold = NULL;
    acr_analysis_control_config_t config = {0};
    esp_err_t err = http_helpers_recv_body(req, body, sizeof(body));

    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Body invalido");
    }

    json = cJSON_Parse(body);
    if (!json) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "JSON invalido");
    }

    err = acr_analysis_control_get_config(&config);
    if (err != ESP_OK) {
        goto cleanup;
    }

    automatic_enabled = cJSON_GetObjectItemCaseSensitive(json, "automatic_enabled");
    automatic_interval_ms = cJSON_GetObjectItemCaseSensitive(json, "automatic_interval_ms");
    capture_duration_seconds = cJSON_GetObjectItemCaseSensitive(json, "capture_duration_seconds");
    digital_gain = cJSON_GetObjectItemCaseSensitive(json, "digital_gain");
    silence_threshold_rms = cJSON_GetObjectItemCaseSensitive(json, "silence_threshold_rms");
    silence_hysteresis_rms = cJSON_GetObjectItemCaseSensitive(json, "silence_hysteresis_rms");
    min_active_ms = cJSON_GetObjectItemCaseSensitive(json, "min_active_ms");
    trigger_mode = cJSON_GetObjectItemCaseSensitive(json, "trigger_mode");
    ai_probability_threshold = cJSON_GetObjectItemCaseSensitive(json, "ai_probability_threshold");
    if (cJSON_IsBool(automatic_enabled)) {
        config.automatic_enabled = cJSON_IsTrue(automatic_enabled);
    }
    if (cJSON_IsNumber(automatic_interval_ms)) {
        config.automatic_interval_ms = (uint32_t)automatic_interval_ms->valuedouble;
    }
    if (cJSON_IsNumber(capture_duration_seconds)) {
        config.capture_duration_seconds = (uint32_t)capture_duration_seconds->valuedouble;
    }
    if (cJSON_IsNumber(digital_gain)) {
        config.digital_gain = digital_gain->valuedouble;
    }
    if (cJSON_IsNumber(silence_threshold_rms)) {
        config.silence_threshold_rms = (uint32_t)silence_threshold_rms->valuedouble;
    }
    if (cJSON_IsNumber(silence_hysteresis_rms)) {
        config.silence_hysteresis_rms = (uint32_t)silence_hysteresis_rms->valuedouble;
    }
    if (cJSON_IsNumber(min_active_ms)) {
        config.min_active_ms = (uint32_t)min_active_ms->valuedouble;
    }
    if (cJSON_IsNumber(trigger_mode)) {
        config.trigger_mode = (acr_trigger_mode_t)trigger_mode->valueint;
    }
    if (cJSON_IsNumber(ai_probability_threshold)) {
        config.ai_probability_threshold = ai_probability_threshold->valuedouble;
    }

    err = acr_analysis_control_save_config(&config);

cleanup:
    cJSON_Delete(json);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, http_error_message(err));
    }

    return httpd_resp_sendstr(req, "Controle ACR salvo.");
}

static esp_err_t api_trigger_output_get_handler(httpd_req_t *req)
{
    acr_trigger_output_status_t status = {0};
    cJSON *json = cJSON_CreateObject();
    esp_err_t err = acr_trigger_output_get_status(&status);

    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    if (err == ESP_OK) {
        cJSON_AddBoolToObject(json, "enabled", status.config.enabled);
        cJSON_AddNumberToObject(json, "gpio", status.config.gpio);
        cJSON_AddNumberToObject(json, "active_level", status.config.active_level);
        cJSON_AddNumberToObject(json, "pulse_ms", status.config.pulse_ms);
        cJSON_AddNumberToObject(json, "last_pulse_at_ms", (double)status.last_pulse_at_ms);
        cJSON_AddStringToObject(json, "last_error",
                                status.last_error == ESP_OK ? "" : esp_err_to_name(status.last_error));
    } else {
        cJSON_AddStringToObject(json, "error", http_error_message(err));
    }

    err = http_helpers_send_json(req, json, err == ESP_OK ? 200 : 500);
    cJSON_Delete(json);
    return err;
}

static esp_err_t api_trigger_output_post_handler(httpd_req_t *req)
{
    char body[192];
    cJSON *json = NULL;
    const cJSON *enabled = NULL;
    const cJSON *gpio = NULL;
    const cJSON *active_level = NULL;
    const cJSON *pulse_ms = NULL;
    acr_trigger_output_status_t status = {0};
    acr_trigger_output_config_t config = {0};
    esp_err_t err = http_helpers_recv_body(req, body, sizeof(body));

    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Body invalido");
    }

    json = cJSON_Parse(body);
    if (!json) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "JSON invalido");
    }

    err = acr_trigger_output_get_status(&status);
    if (err != ESP_OK) {
        goto cleanup;
    }

    config = status.config;
    enabled = cJSON_GetObjectItemCaseSensitive(json, "enabled");
    gpio = cJSON_GetObjectItemCaseSensitive(json, "gpio");
    active_level = cJSON_GetObjectItemCaseSensitive(json, "active_level");
    pulse_ms = cJSON_GetObjectItemCaseSensitive(json, "pulse_ms");

    if (cJSON_IsBool(enabled)) {
        config.enabled = cJSON_IsTrue(enabled);
    }
    if (cJSON_IsNumber(gpio)) {
        config.gpio = gpio->valueint;
    }
    if (cJSON_IsNumber(active_level)) {
        config.active_level = active_level->valueint;
    }
    if (cJSON_IsNumber(pulse_ms)) {
        config.pulse_ms = (uint32_t)pulse_ms->valuedouble;
    }

    err = acr_trigger_output_save_config(&config);

cleanup:
    cJSON_Delete(json);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, err == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "500 Internal Server Error");
        return httpd_resp_sendstr(req, http_error_message(err));
    }

    return httpd_resp_sendstr(req, "Saida BT_NEXT salva.");
}

static esp_err_t api_trigger_output_test_post_handler(httpd_req_t *req)
{
    esp_err_t err = acr_trigger_output_pulse();

    if (err != ESP_OK) {
        httpd_resp_set_status(req, err == ESP_ERR_INVALID_STATE ? "409 Conflict" : "500 Internal Server Error");
        return httpd_resp_sendstr(req, http_error_message(err));
    }

    return httpd_resp_sendstr(req, "Pulso BT_NEXT enviado.");
}

static esp_err_t register_acr_routes(httpd_handle_t server)
{
    httpd_uri_t api_config = {
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = api_config_get_handler,
    };
    httpd_uri_t api_acr = {
        .uri = "/api/acr",
        .method = HTTP_POST,
        .handler = api_acr_post_handler,
    };
    httpd_uri_t api_acr_status = {
        .uri = "/api/acr/status",
        .method = HTTP_GET,
        .handler = api_acr_status_get_handler,
    };
    httpd_uri_t api_acr_run = {
        .uri = "/api/acr/run",
        .method = HTTP_POST,
        .handler = api_acr_run_post_handler,
    };
    httpd_uri_t api_acr_control_get = {
        .uri = "/api/acr/control",
        .method = HTTP_GET,
        .handler = api_acr_control_get_handler,
    };
    httpd_uri_t api_acr_control_post = {
        .uri = "/api/acr/control",
        .method = HTTP_POST,
        .handler = api_acr_control_post_handler,
    };
    httpd_uri_t api_trigger_output_get = {
        .uri = "/api/trigger-output",
        .method = HTTP_GET,
        .handler = api_trigger_output_get_handler,
    };
    httpd_uri_t api_trigger_output_post = {
        .uri = "/api/trigger-output",
        .method = HTTP_POST,
        .handler = api_trigger_output_post_handler,
    };
    httpd_uri_t api_trigger_output_test = {
        .uri = "/api/trigger-output/test",
        .method = HTTP_POST,
        .handler = api_trigger_output_test_post_handler,
    };
    esp_err_t err = httpd_register_uri_handler(server, &api_config);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &api_acr);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &api_acr_status);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &api_acr_run);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &api_acr_control_get);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &api_acr_control_post);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &api_trigger_output_get);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &api_trigger_output_post);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &api_trigger_output_test);
    }

    return err;
}

esp_err_t acr_routes_register_with_portal(void)
{
    return web_portal_register_app_routes(register_acr_routes);
}
