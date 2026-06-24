#include "tele_manifest.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "tele_manifest_internal.h"

typedef struct {
    const tele_manifest_artifact_t *artifact;
    const tele_manifest_request_t *request;
    const tele_manifest_stream_apply_t *apply;
    tele_manifest_sha256_ctx_t *sha_ctx;
    bool started;
} stream_download_ctx_t;

typedef struct {
    FILE *file;
    const tele_manifest_artifact_t *artifact;
    const tele_manifest_request_t *request;
    tele_manifest_sha256_ctx_t *sha_ctx;
} file_download_ctx_t;

static void clear_result(tele_manifest_run_result_t *result)
{
    if (result) {
        memset(result, 0, sizeof(*result));
        result->result = TELE_MANIFEST_RESULT_UNKNOWN;
        result->err = ESP_OK;
    }
}

static void set_result(tele_manifest_run_result_t *result,
                       tele_manifest_result_t state,
                       esp_err_t err,
                       const char *message)
{
    if (!result) {
        return;
    }

    result->result = state;
    result->err = err;
    if (message) {
        snprintf(result->message, sizeof(result->message), "%s", message);
    }
}

static tele_manifest_version_decision_t version_decision(
    const tele_manifest_artifact_t *artifact,
    const tele_manifest_request_t *request)
{
    if (request->version_policy) {
        return request->version_policy(artifact, request->ctx);
    }
    return TELE_MANIFEST_VERSION_APPLY;
}

static esp_err_t verify_artifact_sha(tele_manifest_sha256_ctx_t *sha_ctx,
                                     const tele_manifest_artifact_t *artifact)
{
    uint8_t expected[TELE_MANIFEST_SHA256_BYTES] = {0};
    uint8_t actual[TELE_MANIFEST_SHA256_BYTES] = {0};

    esp_err_t err = tele_manifest_sha256_hex_to_bytes(artifact->sha256_hex, expected);
    if (err != ESP_OK) {
        return err;
    }

    err = tele_manifest_sha256_finish(sha_ctx, actual);
    if (err != ESP_OK) {
        return err;
    }

    return memcmp(expected, actual, sizeof(expected)) == 0 ? ESP_OK : ESP_ERR_INVALID_CRC;
}

static esp_err_t stream_chunk_cb(const uint8_t *data,
                                 size_t data_len,
                                 size_t received,
                                 size_t total,
                                 void *ctx)
{
    stream_download_ctx_t *download = (stream_download_ctx_t *)ctx;

    if (!download->started) {
        esp_err_t err = download->apply->begin(download->artifact, download->request->ctx);
        if (err != ESP_OK) {
            return err;
        }
        download->started = true;
    }

    esp_err_t err = tele_manifest_sha256_update(download->sha_ctx, data, data_len);
    if (err != ESP_OK) {
        return err;
    }

    err = download->apply->write(data, data_len, download->request->ctx);
    if (err != ESP_OK) {
        return err;
    }

    if (download->request->progress) {
        download->request->progress(download->artifact,
                                    received,
                                    total,
                                    download->request->ctx);
    }

    return ESP_OK;
}

static esp_err_t file_chunk_cb(const uint8_t *data,
                               size_t data_len,
                               size_t received,
                               size_t total,
                               void *ctx)
{
    file_download_ctx_t *download = (file_download_ctx_t *)ctx;

    if (fwrite(data, 1, data_len, download->file) != data_len) {
        return ESP_FAIL;
    }

    esp_err_t err = tele_manifest_sha256_update(download->sha_ctx, data, data_len);
    if (err != ESP_OK) {
        return err;
    }

    if (download->request->progress) {
        download->request->progress(download->artifact,
                                    received,
                                    total,
                                    download->request->ctx);
    }

    return ESP_OK;
}

esp_err_t tele_manifest_fetch(const char *manifest_url,
                              const tele_manifest_request_t *request,
                              tele_manifest_artifact_t *out_artifact)
{
    if (!manifest_url || manifest_url[0] == '\0' || !request || !out_artifact) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t max_manifest_size = request->max_manifest_size > 0 ?
                               request->max_manifest_size :
                               CONFIG_TELE_MANIFEST_DEFAULT_MAX_MANIFEST_SIZE;
    if (max_manifest_size < 2) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *manifest_text = calloc(1, max_manifest_size);
    if (!manifest_text) {
        return ESP_ERR_NO_MEM;
    }

    int http_status = 0;
    esp_err_t err = tele_manifest_http_get_text(manifest_url,
                                                manifest_text,
                                                max_manifest_size,
                                                &http_status);
    if (err == ESP_OK) {
        err = tele_manifest_parse_json(manifest_text, request, out_artifact);
    }

    free(manifest_text);
    return err;
}

esp_err_t tele_manifest_apply_stream(const tele_manifest_artifact_t *artifact,
                                     const tele_manifest_request_t *request,
                                     const tele_manifest_stream_apply_t *apply,
                                     tele_manifest_run_result_t *out_result)
{
    if (!artifact || !request || !apply || !apply->begin || !apply->write ||
        !apply->finish || !apply->abort || artifact->url_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    clear_result(out_result);

    esp_err_t last_err = ESP_FAIL;
    for (size_t i = 0; i < artifact->url_count; i++) {
        stream_download_ctx_t download = {
            .artifact = artifact,
            .request = request,
            .apply = apply,
        };

        if (out_result) {
            snprintf(out_result->selected_url,
                     sizeof(out_result->selected_url),
                     "%s",
                     artifact->urls[i]);
        }

        last_err = tele_manifest_sha256_begin(&download.sha_ctx);
        if (last_err != ESP_OK) {
            set_result(out_result, TELE_MANIFEST_RESULT_FAILED, last_err, "SHA-256 init failed");
            return last_err;
        }

        int http_status = 0;
        size_t received = 0;
        last_err = tele_manifest_http_download(artifact->urls[i],
                                               artifact->size,
                                               stream_chunk_cb,
                                               &download,
                                               &http_status,
                                               &received);
        if (out_result) {
            out_result->http_status = http_status;
            out_result->bytes_received = received;
        }

        if (last_err == ESP_OK) {
            last_err = verify_artifact_sha(download.sha_ctx, artifact);
        } else {
            tele_manifest_sha256_abort(download.sha_ctx);
        }

        if (last_err == ESP_OK) {
            last_err = apply->finish(artifact, request->ctx);
        }

        if (last_err == ESP_OK) {
            set_result(out_result, TELE_MANIFEST_RESULT_APPLIED, ESP_OK, "Applied");
            return ESP_OK;
        }

        if (download.started) {
            apply->abort(request->ctx);
        }
    }

    set_result(out_result, TELE_MANIFEST_RESULT_FAILED, last_err, "Artifact stream failed");
    return last_err;
}

esp_err_t tele_manifest_run_stream(const char *manifest_url,
                                   const tele_manifest_request_t *request,
                                   const tele_manifest_stream_apply_t *apply,
                                   tele_manifest_run_result_t *out_result)
{
    if (!manifest_url || !request || !apply) {
        return ESP_ERR_INVALID_ARG;
    }

    clear_result(out_result);

    tele_manifest_artifact_t artifact = {0};
    esp_err_t err = tele_manifest_fetch(manifest_url, request, &artifact);
    if (err != ESP_OK) {
        set_result(out_result, TELE_MANIFEST_RESULT_FAILED, err, "Manifest fetch failed");
        return err;
    }

    tele_manifest_version_decision_t decision = version_decision(&artifact, request);
    if (decision == TELE_MANIFEST_VERSION_SKIP_CURRENT) {
        set_result(out_result, TELE_MANIFEST_RESULT_SKIPPED_CURRENT, ESP_OK, "Current version");
        return ESP_OK;
    }
    if (decision != TELE_MANIFEST_VERSION_APPLY) {
        set_result(out_result, TELE_MANIFEST_RESULT_REJECTED_BY_POLICY, ESP_ERR_INVALID_STATE, "Rejected by policy");
        return ESP_ERR_INVALID_STATE;
    }

    return tele_manifest_apply_stream(&artifact, request, apply, out_result);
}

esp_err_t tele_manifest_apply_file(const tele_manifest_artifact_t *artifact,
                                   const tele_manifest_request_t *request,
                                   const char *work_dir,
                                   tele_manifest_file_apply_cb_t apply,
                                   tele_manifest_run_result_t *out_result)
{
    if (!artifact || !request || !work_dir || work_dir[0] == '\0' || !apply ||
        artifact->url_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    clear_result(out_result);

    char temp_path[128] = {0};
    if (snprintf(temp_path, sizeof(temp_path), "%s/%s", work_dir, "manifest_artifact.tmp") >=
        (int)sizeof(temp_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t last_err = ESP_FAIL;
    for (size_t i = 0; i < artifact->url_count; i++) {
        FILE *file = fopen(temp_path, "wb");
        if (!file) {
            set_result(out_result, TELE_MANIFEST_RESULT_FAILED, ESP_FAIL, "Temp file open failed");
            return ESP_FAIL;
        }

        file_download_ctx_t download = {
            .file = file,
            .artifact = artifact,
            .request = request,
        };

        if (out_result) {
            snprintf(out_result->selected_url,
                     sizeof(out_result->selected_url),
                     "%s",
                     artifact->urls[i]);
        }

        last_err = tele_manifest_sha256_begin(&download.sha_ctx);
        if (last_err != ESP_OK) {
            fclose(file);
            remove(temp_path);
            set_result(out_result, TELE_MANIFEST_RESULT_FAILED, last_err, "SHA-256 init failed");
            return last_err;
        }

        int http_status = 0;
        size_t received = 0;
        last_err = tele_manifest_http_download(artifact->urls[i],
                                               artifact->size,
                                               file_chunk_cb,
                                               &download,
                                               &http_status,
                                               &received);
        if (out_result) {
            out_result->http_status = http_status;
            out_result->bytes_received = received;
        }

        if (last_err == ESP_OK) {
            last_err = verify_artifact_sha(download.sha_ctx, artifact);
        } else {
            tele_manifest_sha256_abort(download.sha_ctx);
        }

        if (fclose(file) != 0 && last_err == ESP_OK) {
            last_err = ESP_FAIL;
        }

        if (last_err == ESP_OK) {
            last_err = apply(temp_path, artifact, request->ctx);
        }

        remove(temp_path);

        if (last_err == ESP_OK) {
            set_result(out_result, TELE_MANIFEST_RESULT_APPLIED, ESP_OK, "Applied");
            return ESP_OK;
        }
    }

    set_result(out_result, TELE_MANIFEST_RESULT_FAILED, last_err, "Artifact file apply failed");
    return last_err;
}

esp_err_t tele_manifest_run_file(const char *manifest_url,
                                 const tele_manifest_request_t *request,
                                 const char *work_dir,
                                 tele_manifest_file_apply_cb_t apply,
                                 tele_manifest_run_result_t *out_result)
{
    if (!manifest_url || !request || !work_dir || !apply) {
        return ESP_ERR_INVALID_ARG;
    }

    clear_result(out_result);

    tele_manifest_artifact_t artifact = {0};
    esp_err_t err = tele_manifest_fetch(manifest_url, request, &artifact);
    if (err != ESP_OK) {
        set_result(out_result, TELE_MANIFEST_RESULT_FAILED, err, "Manifest fetch failed");
        return err;
    }

    tele_manifest_version_decision_t decision = version_decision(&artifact, request);
    if (decision == TELE_MANIFEST_VERSION_SKIP_CURRENT) {
        set_result(out_result, TELE_MANIFEST_RESULT_SKIPPED_CURRENT, ESP_OK, "Current version");
        return ESP_OK;
    }
    if (decision != TELE_MANIFEST_VERSION_APPLY) {
        set_result(out_result, TELE_MANIFEST_RESULT_REJECTED_BY_POLICY, ESP_ERR_INVALID_STATE, "Rejected by policy");
        return ESP_ERR_INVALID_STATE;
    }

    return tele_manifest_apply_file(&artifact, request, work_dir, apply, out_result);
}
