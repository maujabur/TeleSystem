#include "tele_ca_updater.h"

#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "tele_artifacts.h"
#include "tele_ca_store.h"

#define TELE_CA_UPDATER_ARTIFACT_TYPE "ca_bundle"
#define TELE_CA_UPDATER_VERSION_SIZE 64

static const char *TAG = "tele_ca_updater";

typedef struct {
    char current_version[TELE_CA_UPDATER_VERSION_SIZE];
    bool allow_same_version;
} tele_ca_version_ctx_t;

static bool request_valid(const tele_artifact_request_t *request)
{
    return request &&
           request->manifest_url &&
           request->manifest_url[0] != '\0' &&
           request->artifact_type &&
           strcmp(request->artifact_type, TELE_CA_UPDATER_ARTIFACT_TYPE) == 0;
}

static void make_request(const tele_artifact_request_t *config,
                         tele_manifest_version_cb_t version_policy,
                         void *ctx,
                         tele_manifest_request_t *request)
{
    *request = (tele_manifest_request_t) {
        .artifact_type = TELE_CA_UPDATER_ARTIFACT_TYPE,
        .channel = config->channel,
        .max_manifest_size = CONFIG_TELE_CA_UPDATER_MAX_MANIFEST_SIZE,
        .max_artifact_size = CONFIG_TELE_CA_UPDATER_MAX_BUNDLE_SIZE,
        .version_policy = version_policy,
        .ctx = ctx,
    };
}

static tele_manifest_version_decision_t ca_version_policy(
    const tele_manifest_artifact_t *artifact,
    void *ctx)
{
    tele_ca_version_ctx_t *version_ctx = (tele_ca_version_ctx_t *)ctx;

    if (!artifact || !version_ctx) {
        return TELE_MANIFEST_VERSION_REJECT;
    }

    if (version_ctx->current_version[0] != '\0' &&
        strcmp(version_ctx->current_version, artifact->version) == 0) {
        return version_ctx->allow_same_version ?
               TELE_MANIFEST_VERSION_APPLY :
               TELE_MANIFEST_VERSION_SKIP_CURRENT;
    }

    return TELE_MANIFEST_VERSION_APPLY;
}

static esp_err_t apply_ca_file(const char *verified_path,
                               const tele_manifest_artifact_t *artifact,
                               void *ctx)
{
    (void)ctx;

    if (!artifact) {
        return ESP_ERR_INVALID_ARG;
    }

    return tele_ca_store_apply_file(verified_path, artifact->version);
}

static void load_current_version(tele_ca_version_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    ctx->current_version[0] = '\0';
    esp_err_t err = tele_ca_store_get_version(ctx->current_version, sizeof(ctx->current_version));
    if (err != ESP_OK) {
        ctx->current_version[0] = '\0';
    }
}

static esp_err_t check_ca_artifact(const tele_artifact_request_t *config,
                                   tele_artifact_check_result_t *out_result,
                                   void *ctx)
{
    (void)ctx;

    if (!request_valid(config) || !out_result) {
        return ESP_ERR_INVALID_ARG;
    }

    tele_ca_version_ctx_t version_ctx = {0};
    load_current_version(&version_ctx);
    version_ctx.allow_same_version = config->allow_same_version;

    tele_manifest_request_t request = {0};
    make_request(config, NULL, NULL, &request);
    esp_err_t err = tele_manifest_fetch(config->manifest_url, &request, &out_result->artifact);
    if (err != ESP_OK) {
        return err;
    }

    strlcpy(out_result->current_version,
            version_ctx.current_version,
            sizeof(out_result->current_version));
    out_result->available = config->allow_same_version ||
                            strcmp(version_ctx.current_version, out_result->artifact.version) != 0;
    return ESP_OK;
}

static esp_err_t apply_ca_artifact(const tele_artifact_request_t *config,
                                   tele_artifact_apply_result_t *out_result,
                                   void *ctx)
{
    (void)ctx;

    if (!request_valid(config) || !out_result) {
        return ESP_ERR_INVALID_ARG;
    }

    tele_ca_version_ctx_t version_ctx = {0};
    load_current_version(&version_ctx);

    tele_manifest_request_t request = {0};
    make_request(config, ca_version_policy, &version_ctx, &request);

    esp_err_t err = tele_manifest_run_file(config->manifest_url,
                                           &request,
                                           CONFIG_TELE_CA_STORE_BASE_PATH,
                                           apply_ca_file,
                                           &out_result->run);
    out_result->started_async = false;
    if (err == ESP_OK && out_result->run.result == TELE_MANIFEST_RESULT_APPLIED) {
        ESP_LOGI(TAG, "CA update applied from %s", out_result->run.selected_url);
        if (config->restart_on_success) {
            ESP_LOGW(TAG, "CA update applied; restarting");
            esp_restart();
        }
    }

    return err;
}

esp_err_t tele_ca_updater_register_artifact(void)
{
    static const tele_artifact_handler_t handler = {
        .artifact_type = TELE_CA_UPDATER_ARTIFACT_TYPE,
        .label = "CA bundle",
        .mode = TELE_ARTIFACT_MODE_FILE,
        .check = check_ca_artifact,
        .apply = apply_ca_artifact,
    };

    esp_err_t err = tele_artifacts_register(&handler);
    return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
}
