#ifndef FIRMWARE_OTA_H
#define FIRMWARE_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "tele_manifest.h"

typedef enum {
    FIRMWARE_OTA_STATE_IDLE = 0,
    FIRMWARE_OTA_STATE_RUNNING,
    FIRMWARE_OTA_STATE_SUCCESS,
    FIRMWARE_OTA_STATE_FAILED,
} firmware_ota_state_t;

typedef struct {
    firmware_ota_state_t state;
    bool in_progress;
    bool restart_pending;
    char current_version[64];
    char target_version[64];
    char build_id[64];
    char manifest_url[384];
    char artifact_url[384];
    char url[384];
    char last_error[128];
    char running_partition[32];
    char next_update_partition[32];
    size_t bytes_written;
    size_t total_size;
    uint8_t progress_pct;
} firmware_ota_status_t;

typedef struct {
    const char *manifest_url;
    const char *channel;
    bool allow_same_version;
    bool restart_on_success;
} firmware_ota_manifest_config_t;

esp_err_t firmware_ota_init(void);
esp_err_t firmware_ota_register_artifact(void);
esp_err_t firmware_ota_start(const char *url);
esp_err_t firmware_ota_check_manifest(const firmware_ota_manifest_config_t *config,
                                      tele_manifest_artifact_t *out_artifact);
esp_err_t firmware_ota_start_manifest(const firmware_ota_manifest_config_t *config);
esp_err_t firmware_ota_upload_begin(void);
esp_err_t firmware_ota_upload_write(const uint8_t *data, size_t data_len);
esp_err_t firmware_ota_upload_finalize(void);
void firmware_ota_upload_abort(void);
void firmware_ota_get_status(firmware_ota_status_t *status);
const char *firmware_ota_state_name(firmware_ota_state_t state);

#endif
