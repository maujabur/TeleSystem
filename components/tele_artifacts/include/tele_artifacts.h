#ifndef TELE_ARTIFACTS_H
#define TELE_ARTIFACTS_H

#include <stdbool.h>

#include "tele_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TELE_ARTIFACT_TYPE_SIZE TELE_MANIFEST_ARTIFACT_TYPE_SIZE
#define TELE_ARTIFACT_VERSION_SIZE TELE_MANIFEST_VERSION_SIZE

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

typedef esp_err_t (*tele_artifact_check_cb_t)(const tele_artifact_request_t *request,
                                              tele_artifact_check_result_t *out_result,
                                              void *ctx);

typedef esp_err_t (*tele_artifact_apply_cb_t)(const tele_artifact_request_t *request,
                                              tele_artifact_apply_result_t *out_result,
                                              void *ctx);

typedef struct {
    const char *artifact_type;
    const char *label;
    tele_artifact_mode_t mode;
    bool default_restart_on_success;
    tele_artifact_check_cb_t check;
    tele_artifact_apply_cb_t apply;
    void *ctx;
} tele_artifact_handler_t;

esp_err_t tele_artifacts_register(const tele_artifact_handler_t *handler);
const tele_artifact_handler_t *tele_artifacts_find(const char *artifact_type);
esp_err_t tele_artifacts_check(const tele_artifact_request_t *request,
                               tele_artifact_check_result_t *out_result);
esp_err_t tele_artifacts_apply(const tele_artifact_request_t *request,
                               tele_artifact_apply_result_t *out_result);

#ifndef TELE_ARTIFACTS_HOST_TEST
esp_err_t tele_artifacts_register_commands(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
