#include "tele_manifest.h"

esp_err_t tele_manifest_fetch(const char *manifest_url,
                              const tele_manifest_request_t *request,
                              tele_manifest_artifact_t *out_artifact)
{
    (void)manifest_url;
    (void)request;
    (void)out_artifact;
    return ESP_ERR_NOT_SUPPORTED;
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
