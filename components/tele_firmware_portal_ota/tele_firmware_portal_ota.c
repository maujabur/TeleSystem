#include "tele_firmware_portal_ota.h"

#include "cJSON.h"

#include "firmware_ota.h"
#include "tele_portal_ota.h"

#define TELE_FIRMWARE_PORTAL_OTA_RESTART_DELAY_MS 1200

static esp_err_t init_portal_ota_adapter(void);

static esp_err_t portal_ota_begin_cb(void *ctx)
{
    (void)ctx;
    return firmware_ota_upload_begin();
}

static esp_err_t portal_ota_write_cb(const uint8_t *data, size_t data_len, void *ctx)
{
    (void)ctx;
    return firmware_ota_upload_write(data, data_len);
}

static esp_err_t portal_ota_finalize_cb(void *ctx)
{
    (void)ctx;
    return firmware_ota_upload_finalize();
}

static void portal_ota_abort_cb(void *ctx)
{
    (void)ctx;
    firmware_ota_upload_abort();
}

static esp_err_t portal_ota_status_cb(cJSON *json, void *ctx)
{
    (void)ctx;
    firmware_ota_status_t status = {0};

    if (!json) {
        return ESP_ERR_INVALID_ARG;
    }

    firmware_ota_get_status(&status);
    cJSON_AddStringToObject(json, "state", firmware_ota_state_name(status.state));
    cJSON_AddBoolToObject(json, "in_progress", status.in_progress);
    cJSON_AddBoolToObject(json, "restart_pending", status.restart_pending);
    cJSON_AddStringToObject(json, "current_version", status.current_version);
    cJSON_AddStringToObject(json, "target_version", status.target_version);
    cJSON_AddStringToObject(json, "build_id", status.build_id);
    cJSON_AddStringToObject(json, "configured_url", status.url);
    cJSON_AddStringToObject(json, "manifest_url", status.manifest_url);
    cJSON_AddStringToObject(json, "artifact_url", status.artifact_url);
    cJSON_AddStringToObject(json, "last_error", status.last_error);
    cJSON_AddStringToObject(json, "running_partition", status.running_partition);
    cJSON_AddStringToObject(json, "next_update_partition", status.next_update_partition);
    cJSON_AddNumberToObject(json, "bytes_written", (double)status.bytes_written);
    cJSON_AddNumberToObject(json, "total_size", (double)status.total_size);
    cJSON_AddNumberToObject(json, "progress_pct", status.progress_pct);

    return ESP_OK;
}

static esp_err_t init_portal_ota_adapter(void)
{
    const tele_portal_ota_config_t config = {
        .begin = portal_ota_begin_cb,
        .write = portal_ota_write_cb,
        .finalize = portal_ota_finalize_cb,
        .abort = portal_ota_abort_cb,
        .status = portal_ota_status_cb,
        .restart_delay_ms = TELE_FIRMWARE_PORTAL_OTA_RESTART_DELAY_MS,
    };

    return tele_portal_ota_init(&config);
}

esp_err_t tele_firmware_portal_ota_register_handlers(httpd_handle_t server)
{
    esp_err_t err = init_portal_ota_adapter();
    if (err != ESP_OK) {
        return err;
    }

    return tele_portal_ota_register_handlers(server);
}

esp_err_t tele_firmware_portal_ota_register_routes(void)
{
    esp_err_t err = init_portal_ota_adapter();
    if (err != ESP_OK) {
        return err;
    }

    return tele_portal_ota_register_routes();
}
