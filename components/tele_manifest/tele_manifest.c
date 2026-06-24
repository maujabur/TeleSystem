#include "tele_manifest.h"

#include <stdlib.h>

#include "sdkconfig.h"

esp_err_t tele_manifest_http_get_text(const char *url,
                                      char *out_text,
                                      size_t out_size,
                                      int *out_status);

esp_err_t tele_manifest_parse_json(const char *text,
                                   const tele_manifest_request_t *request,
                                   tele_manifest_artifact_t *out_artifact);

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
    (void)artifact;
    (void)request;
    (void)apply;
    (void)out_result;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t tele_manifest_run_stream(const char *manifest_url,
                                   const tele_manifest_request_t *request,
                                   const tele_manifest_stream_apply_t *apply,
                                   tele_manifest_run_result_t *out_result)
{
    (void)manifest_url;
    (void)request;
    (void)apply;
    (void)out_result;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t tele_manifest_apply_file(const tele_manifest_artifact_t *artifact,
                                   const tele_manifest_request_t *request,
                                   const char *work_dir,
                                   tele_manifest_file_apply_cb_t apply,
                                   tele_manifest_run_result_t *out_result)
{
    (void)artifact;
    (void)request;
    (void)work_dir;
    (void)apply;
    (void)out_result;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t tele_manifest_run_file(const char *manifest_url,
                                 const tele_manifest_request_t *request,
                                 const char *work_dir,
                                 tele_manifest_file_apply_cb_t apply,
                                 tele_manifest_run_result_t *out_result)
{
    (void)manifest_url;
    (void)request;
    (void)work_dir;
    (void)apply;
    (void)out_result;
    return ESP_ERR_NOT_SUPPORTED;
}
