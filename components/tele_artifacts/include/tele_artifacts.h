#ifndef TELE_ARTIFACTS_H
#define TELE_ARTIFACTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "tele_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TELE_ARTIFACT_TYPE_SIZE TELE_MANIFEST_ARTIFACT_TYPE_SIZE
#define TELE_ARTIFACT_VERSION_SIZE TELE_MANIFEST_VERSION_SIZE
#define TELE_ARTIFACT_STATE_SIZE 24
#define TELE_ARTIFACT_ERROR_SIZE TELE_MANIFEST_ERROR_SIZE

typedef enum {
    TELE_ARTIFACT_MODE_FILE = 0,
    TELE_ARTIFACT_MODE_STREAM,
} tele_artifact_mode_t;

typedef struct {
    const char *artifact_type;
    const char *manifest_url;
    const char *channel;
    bool allow_same_version;
    bool restart_on_success;
} tele_artifact_request_t;

typedef struct {
    tele_manifest_artifact_t artifact;
    char current_version[TELE_ARTIFACT_VERSION_SIZE];
    bool available;
} tele_artifact_check_result_t;

typedef struct {
    tele_manifest_run_result_t run;
    bool started_async;
} tele_artifact_apply_result_t;

typedef struct {
    char artifact_type[TELE_ARTIFACT_TYPE_SIZE];
    char state[TELE_ARTIFACT_STATE_SIZE];
    char current_version[TELE_ARTIFACT_VERSION_SIZE];
    char target_version[TELE_ARTIFACT_VERSION_SIZE];
    char last_error[TELE_ARTIFACT_ERROR_SIZE];
    bool in_progress;
    size_t bytes_done;
    size_t total_size;
    uint8_t progress_pct;
} tele_artifact_status_t;

typedef esp_err_t (*tele_artifact_check_cb_t)(const tele_artifact_request_t *request,
                                              tele_artifact_check_result_t *out_result,
                                              void *ctx);

typedef esp_err_t (*tele_artifact_apply_cb_t)(const tele_artifact_request_t *request,
                                              tele_artifact_apply_result_t *out_result,
                                              void *ctx);

typedef esp_err_t (*tele_artifact_status_cb_t)(tele_artifact_status_t *out_status,
                                               void *ctx);

typedef struct {
    const char *artifact_type;
    const char *label;
    tele_artifact_mode_t mode;
    bool default_restart_on_success;
    tele_artifact_check_cb_t check;
    tele_artifact_apply_cb_t apply;
    tele_artifact_status_cb_t status;
    void *ctx;
} tele_artifact_handler_t;

esp_err_t tele_artifacts_register(const tele_artifact_handler_t *handler);
const tele_artifact_handler_t *tele_artifacts_find(const char *artifact_type);
esp_err_t tele_artifacts_check(const tele_artifact_request_t *request,
                               tele_artifact_check_result_t *out_result);
esp_err_t tele_artifacts_apply(const tele_artifact_request_t *request,
                               tele_artifact_apply_result_t *out_result);
esp_err_t tele_artifacts_get_status(const char *artifact_type,
                                    tele_artifact_status_t *out_status);
esp_err_t tele_artifacts_add_manifest_to_json(cJSON *root);
esp_err_t tele_artifacts_add_status_to_json(cJSON *root);

#ifndef TELE_ARTIFACTS_HOST_TEST
esp_err_t tele_artifacts_register_commands(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
