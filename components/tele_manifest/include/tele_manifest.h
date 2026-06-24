#ifndef TELE_MANIFEST_H
#define TELE_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef TELE_MANIFEST_HOST_TEST
#include "tele_manifest_host_stubs.h"
#else
#include "esp_err.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define TELE_MANIFEST_ARTIFACT_TYPE_SIZE 32
#define TELE_MANIFEST_CHANNEL_SIZE 32
#define TELE_MANIFEST_VERSION_SIZE 32
#define TELE_MANIFEST_BUILD_ID_SIZE 64
#define TELE_MANIFEST_URL_SIZE 384
#define TELE_MANIFEST_MAX_URLS 4
#define TELE_MANIFEST_SHA256_HEX_SIZE 65
#define TELE_MANIFEST_ERROR_SIZE 128

typedef struct {
    int schema;
    char artifact_type[TELE_MANIFEST_ARTIFACT_TYPE_SIZE];
    char channel[TELE_MANIFEST_CHANNEL_SIZE];
    char version[TELE_MANIFEST_VERSION_SIZE];
    char build_id[TELE_MANIFEST_BUILD_ID_SIZE];
    char urls[TELE_MANIFEST_MAX_URLS][TELE_MANIFEST_URL_SIZE];
    size_t url_count;
    char sha256_hex[TELE_MANIFEST_SHA256_HEX_SIZE];
    size_t size;
    char min_version[TELE_MANIFEST_VERSION_SIZE];
    bool critical;
} tele_manifest_artifact_t;

typedef enum {
    TELE_MANIFEST_RESULT_UNKNOWN = 0,
    TELE_MANIFEST_RESULT_SKIPPED_CURRENT,
    TELE_MANIFEST_RESULT_APPLIED,
    TELE_MANIFEST_RESULT_REJECTED_BY_POLICY,
    TELE_MANIFEST_RESULT_FAILED,
} tele_manifest_result_t;

typedef enum {
    TELE_MANIFEST_VERSION_REJECT = 0,
    TELE_MANIFEST_VERSION_SKIP_CURRENT,
    TELE_MANIFEST_VERSION_APPLY,
} tele_manifest_version_decision_t;

typedef struct {
    tele_manifest_result_t result;
    esp_err_t err;
    int http_status;
    size_t bytes_received;
    char selected_url[TELE_MANIFEST_URL_SIZE];
    char message[TELE_MANIFEST_ERROR_SIZE];
} tele_manifest_run_result_t;

typedef tele_manifest_version_decision_t (*tele_manifest_version_cb_t)(
    const tele_manifest_artifact_t *artifact,
    void *ctx);

typedef esp_err_t (*tele_manifest_file_apply_cb_t)(
    const char *verified_path,
    const tele_manifest_artifact_t *artifact,
    void *ctx);

typedef esp_err_t (*tele_manifest_stream_begin_cb_t)(
    const tele_manifest_artifact_t *artifact,
    void *ctx);

typedef esp_err_t (*tele_manifest_stream_write_cb_t)(
    const uint8_t *data,
    size_t data_len,
    void *ctx);

typedef esp_err_t (*tele_manifest_stream_finish_cb_t)(
    const tele_manifest_artifact_t *artifact,
    void *ctx);

typedef void (*tele_manifest_stream_abort_cb_t)(void *ctx);

typedef void (*tele_manifest_progress_cb_t)(
    const tele_manifest_artifact_t *artifact,
    size_t received,
    size_t total,
    void *ctx);

typedef struct {
    const char *artifact_type;
    const char *channel;
    size_t max_manifest_size;
    size_t max_artifact_size;
    tele_manifest_version_cb_t version_policy;
    tele_manifest_progress_cb_t progress;
    void *ctx;
} tele_manifest_request_t;

typedef struct {
    tele_manifest_stream_begin_cb_t begin;
    tele_manifest_stream_write_cb_t write;
    tele_manifest_stream_finish_cb_t finish;
    tele_manifest_stream_abort_cb_t abort;
} tele_manifest_stream_apply_t;

esp_err_t tele_manifest_fetch(const char *manifest_url,
                              const tele_manifest_request_t *request,
                              tele_manifest_artifact_t *out_artifact);

esp_err_t tele_manifest_apply_stream(const tele_manifest_artifact_t *artifact,
                                     const tele_manifest_request_t *request,
                                     const tele_manifest_stream_apply_t *apply,
                                     tele_manifest_run_result_t *out_result);

esp_err_t tele_manifest_run_stream(const char *manifest_url,
                                   const tele_manifest_request_t *request,
                                   const tele_manifest_stream_apply_t *apply,
                                   tele_manifest_run_result_t *out_result);

esp_err_t tele_manifest_apply_file(const tele_manifest_artifact_t *artifact,
                                   const tele_manifest_request_t *request,
                                   const char *work_dir,
                                   tele_manifest_file_apply_cb_t apply,
                                   tele_manifest_run_result_t *out_result);

esp_err_t tele_manifest_run_file(const char *manifest_url,
                                 const tele_manifest_request_t *request,
                                 const char *work_dir,
                                 tele_manifest_file_apply_cb_t apply,
                                 tele_manifest_run_result_t *out_result);

#ifdef __cplusplus
}
#endif

#endif
