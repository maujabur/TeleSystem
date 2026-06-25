#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "tele_artifacts.h"

static int check_calls;
static int apply_calls;

static esp_err_t handle_check(const tele_artifact_request_t *request,
                              tele_artifact_check_result_t *out_result,
                              void *ctx)
{
    const char *expected_type = (const char *)ctx;

    assert(request != NULL);
    assert(out_result != NULL);
    assert(strcmp(request->artifact_type, expected_type) == 0);
    assert(strcmp(request->manifest_url, "https://updates.example.com/manifest.json") == 0);
    assert(strcmp(request->channel, "pilot") == 0);
    assert(request->allow_same_version);
    assert(request->restart_on_success);

    check_calls++;
    strcpy(out_result->current_version, "1.0.0");
    out_result->available = true;
    strcpy(out_result->artifact.artifact_type, expected_type);
    strcpy(out_result->artifact.version, "1.1.0");
    return ESP_OK;
}

static esp_err_t handle_apply(const tele_artifact_request_t *request,
                              tele_artifact_apply_result_t *out_result,
                              void *ctx)
{
    const char *expected_type = (const char *)ctx;

    assert(request != NULL);
    assert(out_result != NULL);
    assert(strcmp(request->artifact_type, expected_type) == 0);
    assert(strcmp(request->manifest_url, "https://updates.example.com/manifest.json") == 0);

    apply_calls++;
    out_result->started_async = true;
    out_result->run.result = TELE_MANIFEST_RESULT_UNKNOWN;
    strcpy(out_result->run.message, "started");
    return ESP_OK;
}

static const tele_artifact_handler_t handlers[] = {
    {
        .artifact_type = "firmware",
        .label = "Firmware",
        .mode = TELE_ARTIFACT_MODE_STREAM,
        .check = handle_check,
        .apply = handle_apply,
        .ctx = "firmware",
    },
    {
        .artifact_type = "ca_bundle",
        .label = "CA bundle",
        .mode = TELE_ARTIFACT_MODE_FILE,
        .check = handle_check,
        .apply = handle_apply,
        .ctx = "ca_bundle",
    },
};

int main(void)
{
    assert(tele_artifacts_register(&handlers[0]) == ESP_OK);
    assert(tele_artifacts_register(&handlers[1]) == ESP_OK);
    assert(tele_artifacts_register(&handlers[0]) == ESP_ERR_INVALID_STATE);
    assert(tele_artifacts_find("firmware") == &handlers[0]);
    assert(tele_artifacts_find("ca_bundle") == &handlers[1]);
    assert(tele_artifacts_find("missing") == NULL);

    const tele_artifact_request_t request = {
        .artifact_type = "firmware",
        .manifest_url = "https://updates.example.com/manifest.json",
        .channel = "pilot",
        .allow_same_version = true,
        .restart_on_success = true,
    };
    tele_artifact_check_result_t check_result = {0};
    assert(tele_artifacts_check(&request, &check_result) == ESP_OK);
    assert(check_calls == 1);
    assert(check_result.available);
    assert(strcmp(check_result.current_version, "1.0.0") == 0);
    assert(strcmp(check_result.artifact.artifact_type, "firmware") == 0);

    tele_artifact_apply_result_t apply_result = {0};
    assert(tele_artifacts_apply(&request, &apply_result) == ESP_OK);
    assert(apply_calls == 1);
    assert(apply_result.started_async);
    assert(strcmp(apply_result.run.message, "started") == 0);

    const tele_artifact_request_t missing_request = {
        .artifact_type = "missing",
        .manifest_url = "https://updates.example.com/manifest.json",
    };
    assert(tele_artifacts_check(&missing_request, &check_result) == ESP_ERR_NOT_FOUND);
    assert(tele_artifacts_apply(&missing_request, &apply_result) == ESP_ERR_NOT_FOUND);

    return 0;
}
