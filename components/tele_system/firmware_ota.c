#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "firmware_ota.h"
#include "firmware_version.h"

#ifndef CONFIG_FIRMWARE_OTA_MANIFEST_MAX_SIZE
#define CONFIG_FIRMWARE_OTA_MANIFEST_MAX_SIZE 4096
#endif

#define FIRMWARE_OTA_URL_BUFFER_SIZE 384
#define FIRMWARE_OTA_ERROR_BUFFER_SIZE 128
#define FIRMWARE_OTA_PARTITION_LABEL_SIZE 32
#define FIRMWARE_OTA_TASK_STACK_SIZE 10240
#define FIRMWARE_OTA_TASK_PRIORITY 5
#define FIRMWARE_OTA_ARTIFACT_TYPE "firmware"
#define FIRMWARE_OTA_VERSION_BUFFER_SIZE 64

static const char *TAG = "firmware-ota";

typedef struct {
    char manifest_url[FIRMWARE_OTA_URL_BUFFER_SIZE];
    char channel[TELE_MANIFEST_CHANNEL_SIZE];
    bool allow_same_version;
    bool restart_on_success;
} firmware_ota_manifest_task_arg_t;

typedef struct {
    const firmware_ota_manifest_task_arg_t *config;
} firmware_ota_manifest_policy_ctx_t;

static portMUX_TYPE s_ota_lock = portMUX_INITIALIZER_UNLOCKED;
static firmware_ota_status_t s_status;
static esp_ota_handle_t s_upload_handle;
static const esp_partition_t *s_upload_partition;
static bool s_upload_active;

static bool copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return false;
    }

    dst[0] = '\0';
    if (!src || src[0] == '\0') {
        return true;
    }

    if (snprintf(dst, dst_size, "%s", src) >= (int)dst_size) {
        dst[0] = '\0';
        return false;
    }

    return true;
}

static bool ota_url_valid(const char *url)
{
    size_t len = 0;

    if (!url) {
        return false;
    }

    len = strlen(url);
    if (len < strlen("https://a.bin")) {
        return false;
    }

    return strncmp(url, "https://", 8) == 0 && strstr(url, ".bin") != NULL;
}

static int parse_semver_part(const char **text)
{
    const char *cursor = *text;
    int value = 0;

    while (*cursor >= '0' && *cursor <= '9') {
        value = (value * 10) + (*cursor - '0');
        cursor++;
    }

    *text = cursor;
    return value;
}

static bool semver_valid(const char *version)
{
    const char *cursor = version;

    if (!cursor || *cursor == '\0') {
        return false;
    }

    for (int part = 0; part < 3; part++) {
        if (*cursor < '0' || *cursor > '9') {
            return false;
        }
        while (*cursor >= '0' && *cursor <= '9') {
            cursor++;
        }
        if (part < 2) {
            if (*cursor != '.') {
                return false;
            }
            cursor++;
        }
    }

    return *cursor == '\0';
}

static int semver_compare(const char *left, const char *right)
{
    const char *a = left ? left : "";
    const char *b = right ? right : "";

    for (int i = 0; i < 3; i++) {
        int av = parse_semver_part(&a);
        int bv = parse_semver_part(&b);

        if (av != bv) {
            return av > bv ? 1 : -1;
        }

        if (*a == '.') {
            a++;
        }
        if (*b == '.') {
            b++;
        }
    }

    return 0;
}

static size_t next_update_partition_size(void)
{
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    return next ? next->size : 0;
}

static void refresh_partition_labels(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    portENTER_CRITICAL(&s_ota_lock);
    copy_text(s_status.running_partition,
              sizeof(s_status.running_partition),
              running ? running->label : "");
    copy_text(s_status.next_update_partition,
              sizeof(s_status.next_update_partition),
              next ? next->label : "");
    portEXIT_CRITICAL(&s_ota_lock);
}

static void set_state(firmware_ota_state_t state, bool in_progress, bool restart_pending)
{
    portENTER_CRITICAL(&s_ota_lock);
    s_status.state = state;
    s_status.in_progress = in_progress;
    s_status.restart_pending = restart_pending;
    portEXIT_CRITICAL(&s_ota_lock);
}

static void set_last_error_text(const char *text)
{
    portENTER_CRITICAL(&s_ota_lock);
    copy_text(s_status.last_error, sizeof(s_status.last_error), text);
    portEXIT_CRITICAL(&s_ota_lock);
}

static void set_last_error_err(esp_err_t err)
{
    set_last_error_text(esp_err_to_name(err));
}

static void reset_transfer_status_locked(void)
{
    s_status.target_version[0] = '\0';
    s_status.build_id[0] = '\0';
    s_status.manifest_url[0] = '\0';
    s_status.artifact_url[0] = '\0';
    s_status.bytes_written = 0;
    s_status.total_size = 0;
    s_status.progress_pct = 0;
}

static bool last_error_empty(void)
{
    portENTER_CRITICAL(&s_ota_lock);
    bool empty = s_status.last_error[0] == '\0';
    portEXIT_CRITICAL(&s_ota_lock);
    return empty;
}

static void update_transfer_progress(size_t bytes_written, size_t total_size)
{
    portENTER_CRITICAL(&s_ota_lock);
    s_status.bytes_written = bytes_written;
    s_status.total_size = total_size;
    s_status.progress_pct = total_size > 0 ?
                            (uint8_t)((bytes_written * 100U) / total_size) :
                            0;
    portEXIT_CRITICAL(&s_ota_lock);
}

static void make_manifest_request(const firmware_ota_manifest_task_arg_t *config,
                                  tele_manifest_version_cb_t version_policy,
                                  void *ctx,
                                  tele_manifest_request_t *request)
{
    size_t max_artifact_size = next_update_partition_size();

    *request = (tele_manifest_request_t) {
        .artifact_type = FIRMWARE_OTA_ARTIFACT_TYPE,
        .channel = config->channel[0] != '\0' ? config->channel : NULL,
        .max_manifest_size = CONFIG_FIRMWARE_OTA_MANIFEST_MAX_SIZE,
        .max_artifact_size = max_artifact_size,
        .version_policy = version_policy,
        .ctx = ctx,
    };
}

static tele_manifest_version_decision_t firmware_manifest_version_policy(
    const tele_manifest_artifact_t *artifact,
    void *ctx)
{
    firmware_ota_manifest_policy_ctx_t *policy_ctx = (firmware_ota_manifest_policy_ctx_t *)ctx;

    if (!artifact || !policy_ctx || !policy_ctx->config) {
        set_last_error_text("INVALID_VERSION_POLICY_CTX");
        return TELE_MANIFEST_VERSION_REJECT;
    }

    if (!semver_valid(APP_VERSION_SEMVER) || !semver_valid(artifact->version) ||
        (artifact->min_version[0] != '\0' && !semver_valid(artifact->min_version))) {
        set_last_error_text("INVALID_SEMVER");
        return TELE_MANIFEST_VERSION_REJECT;
    }

    if (artifact->min_version[0] != '\0' &&
        semver_compare(APP_VERSION_SEMVER, artifact->min_version) < 0) {
        set_last_error_text("CURRENT_VERSION_BELOW_MIN_VERSION");
        return TELE_MANIFEST_VERSION_REJECT;
    }

    size_t partition_size = next_update_partition_size();
    if (partition_size == 0 || artifact->size > partition_size) {
        set_last_error_text("ARTIFACT_TOO_LARGE_FOR_OTA_PARTITION");
        return TELE_MANIFEST_VERSION_REJECT;
    }

    int version_cmp = semver_compare(artifact->version, APP_VERSION_SEMVER);
    if (version_cmp == 0) {
        return policy_ctx->config->allow_same_version ?
               TELE_MANIFEST_VERSION_APPLY :
               TELE_MANIFEST_VERSION_SKIP_CURRENT;
    }

    if (version_cmp > 0) {
        return TELE_MANIFEST_VERSION_APPLY;
    }

    set_last_error_text("TARGET_VERSION_OLDER_THAN_CURRENT");
    return TELE_MANIFEST_VERSION_REJECT;
}

static void firmware_ota_task(void *arg)
{
    char url[FIRMWARE_OTA_URL_BUFFER_SIZE] = {0};
    esp_http_client_config_t http_config = {0};
    esp_https_ota_config_t ota_config = {0};

    if (arg) {
        copy_text(url, sizeof(url), (const char *)arg);
        vPortFree(arg);
    }

    ESP_LOGI(TAG, "Iniciando OTA por %s", url);

    http_config.url = url;
    http_config.timeout_ms = 15000;
    http_config.keep_alive_enable = true;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;

    ota_config.http_config = &http_config;

    esp_err_t err = esp_https_ota(&ota_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA falhou: %s", esp_err_to_name(err));
        set_last_error_err(err);
        set_state(FIRMWARE_OTA_STATE_FAILED, false, false);
        refresh_partition_labels();
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGW(TAG, "OTA concluida com sucesso; reiniciando para ativar novo firmware");
    set_last_error_text("");
    set_state(FIRMWARE_OTA_STATE_SUCCESS, false, true);
    refresh_partition_labels();
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

esp_err_t firmware_ota_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = FIRMWARE_OTA_STATE_IDLE;
    copy_text(s_status.current_version, sizeof(s_status.current_version), APP_VERSION_STRING);
    refresh_partition_labels();
    return ESP_OK;
}

esp_err_t firmware_ota_start(const char *url)
{
    char *task_url = NULL;

    if (!ota_url_valid(url)) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_ota_lock);
    if (s_status.in_progress) {
        portEXIT_CRITICAL(&s_ota_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_status.in_progress = true;
    s_status.restart_pending = false;
    s_status.state = FIRMWARE_OTA_STATE_RUNNING;
    reset_transfer_status_locked();
    copy_text(s_status.url, sizeof(s_status.url), url);
    copy_text(s_status.artifact_url, sizeof(s_status.artifact_url), url);
    s_status.last_error[0] = '\0';
    portEXIT_CRITICAL(&s_ota_lock);

    task_url = pvPortMalloc(FIRMWARE_OTA_URL_BUFFER_SIZE);
    if (!task_url) {
        set_last_error_text("NO_MEM");
        set_state(FIRMWARE_OTA_STATE_FAILED, false, false);
        return ESP_ERR_NO_MEM;
    }

    copy_text(task_url, FIRMWARE_OTA_URL_BUFFER_SIZE, url);
    BaseType_t task_ok = xTaskCreate(firmware_ota_task,
                                     "firmware_ota",
                                     FIRMWARE_OTA_TASK_STACK_SIZE,
                                     task_url,
                                     FIRMWARE_OTA_TASK_PRIORITY,
                                     NULL);
    if (task_ok != pdPASS) {
        vPortFree(task_url);
        set_last_error_text("TASK_CREATE_FAILED");
        set_state(FIRMWARE_OTA_STATE_FAILED, false, false);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t manifest_stream_begin(const tele_manifest_artifact_t *artifact, void *ctx)
{
    (void)ctx;

    if (!artifact) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *next_partition = esp_ota_get_next_update_partition(NULL);
    if (!next_partition) {
        set_last_error_text("NEXT_PARTITION_NOT_FOUND");
        return ESP_ERR_NOT_FOUND;
    }

    if (artifact->size == 0 || artifact->size > next_partition->size) {
        set_last_error_text("ARTIFACT_TOO_LARGE_FOR_OTA_PARTITION");
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_ota_begin(next_partition, artifact->size, &s_upload_handle);
    if (err != ESP_OK) {
        set_last_error_err(err);
        return err;
    }

    portENTER_CRITICAL(&s_ota_lock);
    s_upload_active = true;
    s_upload_partition = next_partition;
    copy_text(s_status.target_version, sizeof(s_status.target_version), artifact->version);
    copy_text(s_status.build_id, sizeof(s_status.build_id), artifact->build_id);
    copy_text(s_status.artifact_url,
              sizeof(s_status.artifact_url),
              artifact->url_count > 0 ? artifact->urls[0] : "");
    s_status.total_size = artifact->size;
    s_status.bytes_written = 0;
    s_status.progress_pct = 0;
    portEXIT_CRITICAL(&s_ota_lock);

    refresh_partition_labels();
    ESP_LOGI(TAG,
             "Manifest OTA iniciado para particao %s, versao %s, %u bytes",
             next_partition->label,
             artifact->version,
             (unsigned)artifact->size);
    return ESP_OK;
}

static esp_err_t manifest_stream_write(const uint8_t *data, size_t data_len, void *ctx)
{
    (void)ctx;

    if (!data || data_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_ota_lock);
    bool upload_active = s_upload_active;
    size_t bytes_written = s_status.bytes_written;
    size_t total_size = s_status.total_size;
    portEXIT_CRITICAL(&s_ota_lock);

    if (!upload_active) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_ota_write(s_upload_handle, data, data_len);
    if (err != ESP_OK) {
        set_last_error_err(err);
        return err;
    }

    update_transfer_progress(bytes_written + data_len, total_size);
    return ESP_OK;
}

static esp_err_t manifest_stream_finish(const tele_manifest_artifact_t *artifact, void *ctx)
{
    (void)ctx;
    esp_err_t err = ESP_OK;

    portENTER_CRITICAL(&s_ota_lock);
    bool upload_active = s_upload_active;
    const esp_partition_t *upload_partition = s_upload_partition;
    s_upload_active = false;
    s_upload_partition = NULL;
    portEXIT_CRITICAL(&s_ota_lock);

    if (!upload_active || !upload_partition || !artifact) {
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_ota_end(s_upload_handle);
    if (err != ESP_OK) {
        set_last_error_err(err);
        set_state(FIRMWARE_OTA_STATE_FAILED, false, false);
        return err;
    }

    err = esp_ota_set_boot_partition(upload_partition);
    if (err != ESP_OK) {
        set_last_error_err(err);
        set_state(FIRMWARE_OTA_STATE_FAILED, false, false);
        return err;
    }

    portENTER_CRITICAL(&s_ota_lock);
    copy_text(s_status.target_version, sizeof(s_status.target_version), artifact->version);
    copy_text(s_status.build_id, sizeof(s_status.build_id), artifact->build_id);
    s_status.bytes_written = artifact->size;
    s_status.total_size = artifact->size;
    s_status.progress_pct = 100;
    portEXIT_CRITICAL(&s_ota_lock);

    set_last_error_text("");
    set_state(FIRMWARE_OTA_STATE_SUCCESS, false, true);
    refresh_partition_labels();
    ESP_LOGW(TAG, "Manifest OTA concluido com sucesso para %s", upload_partition->label);
    return ESP_OK;
}

static void manifest_stream_abort(void *ctx)
{
    (void)ctx;

    portENTER_CRITICAL(&s_ota_lock);
    bool upload_active = s_upload_active;
    s_upload_active = false;
    s_upload_partition = NULL;
    portEXIT_CRITICAL(&s_ota_lock);

    if (upload_active) {
        (void)esp_ota_abort(s_upload_handle);
    }

    set_state(FIRMWARE_OTA_STATE_FAILED, false, false);
    refresh_partition_labels();
}

static void manifest_stream_progress(const tele_manifest_artifact_t *artifact,
                                     size_t received,
                                     size_t total,
                                     void *ctx)
{
    (void)artifact;
    (void)ctx;
    update_transfer_progress(received, total);
}

static void firmware_ota_manifest_task(void *arg)
{
    firmware_ota_manifest_task_arg_t *config = (firmware_ota_manifest_task_arg_t *)arg;
    tele_manifest_run_result_t result = {0};
    firmware_ota_manifest_policy_ctx_t policy_ctx = {
        .config = config,
    };
    tele_manifest_request_t request = {0};
    tele_manifest_stream_apply_t apply = {
        .begin = manifest_stream_begin,
        .write = manifest_stream_write,
        .finish = manifest_stream_finish,
        .abort = manifest_stream_abort,
    };

    make_manifest_request(config, firmware_manifest_version_policy, &policy_ctx, &request);
    request.progress = manifest_stream_progress;

    ESP_LOGI(TAG, "Iniciando OTA por manifest %s", config->manifest_url);
    esp_err_t err = tele_manifest_run_stream(config->manifest_url, &request, &apply, &result);

    portENTER_CRITICAL(&s_ota_lock);
    copy_text(s_status.artifact_url, sizeof(s_status.artifact_url), result.selected_url);
    portEXIT_CRITICAL(&s_ota_lock);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA por manifest falhou: %s (%s)", esp_err_to_name(err), result.message);
        if (last_error_empty()) {
            set_last_error_err(err);
        }
        set_state(FIRMWARE_OTA_STATE_FAILED, false, false);
        refresh_partition_labels();
        vPortFree(config);
        vTaskDelete(NULL);
        return;
    }

    if (result.result == TELE_MANIFEST_RESULT_SKIPPED_CURRENT) {
        ESP_LOGI(TAG, "OTA por manifest ignorado: firmware atual ja corresponde ao manifest");
        set_last_error_text("");
        set_state(FIRMWARE_OTA_STATE_IDLE, false, false);
        refresh_partition_labels();
        vPortFree(config);
        vTaskDelete(NULL);
        return;
    }

    if (config->restart_on_success) {
        ESP_LOGW(TAG, "OTA por manifest concluido; reiniciando para ativar novo firmware");
        vTaskDelay(pdMS_TO_TICKS(1200));
        esp_restart();
    }

    vPortFree(config);
    vTaskDelete(NULL);
}

esp_err_t firmware_ota_check_manifest(const firmware_ota_manifest_config_t *config,
                                      tele_manifest_artifact_t *out_artifact)
{
    if (!config || !config->manifest_url || config->manifest_url[0] == '\0' || !out_artifact) {
        return ESP_ERR_INVALID_ARG;
    }

    firmware_ota_manifest_task_arg_t normalized = {0};
    if (!copy_text(normalized.manifest_url, sizeof(normalized.manifest_url), config->manifest_url)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (config->channel &&
        !copy_text(normalized.channel, sizeof(normalized.channel), config->channel)) {
        return ESP_ERR_INVALID_SIZE;
    }
    normalized.allow_same_version = config->allow_same_version;
    normalized.restart_on_success = config->restart_on_success;

    firmware_ota_manifest_policy_ctx_t policy_ctx = {
        .config = &normalized,
    };
    tele_manifest_request_t request = {0};
    make_manifest_request(&normalized, firmware_manifest_version_policy, &policy_ctx, &request);

    esp_err_t err = tele_manifest_fetch(config->manifest_url, &request, out_artifact);
    if (err != ESP_OK) {
        return err;
    }

    tele_manifest_version_decision_t decision =
        firmware_manifest_version_policy(out_artifact, &policy_ctx);
    return decision == TELE_MANIFEST_VERSION_REJECT ? ESP_ERR_INVALID_STATE : ESP_OK;
}

esp_err_t firmware_ota_start_manifest(const firmware_ota_manifest_config_t *config)
{
    if (!config || !config->manifest_url || config->manifest_url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    firmware_ota_manifest_task_arg_t *task_config =
        pvPortMalloc(sizeof(firmware_ota_manifest_task_arg_t));
    if (!task_config) {
        return ESP_ERR_NO_MEM;
    }
    memset(task_config, 0, sizeof(*task_config));

    if (!copy_text(task_config->manifest_url, sizeof(task_config->manifest_url), config->manifest_url) ||
        (config->channel &&
         !copy_text(task_config->channel, sizeof(task_config->channel), config->channel))) {
        vPortFree(task_config);
        return ESP_ERR_INVALID_SIZE;
    }
    task_config->allow_same_version = config->allow_same_version;
    task_config->restart_on_success = config->restart_on_success;

    portENTER_CRITICAL(&s_ota_lock);
    if (s_status.in_progress || s_upload_active) {
        portEXIT_CRITICAL(&s_ota_lock);
        vPortFree(task_config);
        return ESP_ERR_INVALID_STATE;
    }

    s_status.in_progress = true;
    s_status.restart_pending = false;
    s_status.state = FIRMWARE_OTA_STATE_RUNNING;
    reset_transfer_status_locked();
    copy_text(s_status.url, sizeof(s_status.url), task_config->manifest_url);
    copy_text(s_status.manifest_url, sizeof(s_status.manifest_url), task_config->manifest_url);
    s_status.last_error[0] = '\0';
    portEXIT_CRITICAL(&s_ota_lock);

    BaseType_t task_ok = xTaskCreate(firmware_ota_manifest_task,
                                     "fw_ota_manifest",
                                     FIRMWARE_OTA_TASK_STACK_SIZE,
                                     task_config,
                                     FIRMWARE_OTA_TASK_PRIORITY,
                                     NULL);
    if (task_ok != pdPASS) {
        vPortFree(task_config);
        set_last_error_text("TASK_CREATE_FAILED");
        set_state(FIRMWARE_OTA_STATE_FAILED, false, false);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t firmware_ota_upload_begin(void)
{
    const esp_partition_t *next_partition = NULL;
    esp_err_t err = ESP_OK;

    portENTER_CRITICAL(&s_ota_lock);
    if (s_status.in_progress || s_upload_active) {
        portEXIT_CRITICAL(&s_ota_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_status.in_progress = true;
    s_status.restart_pending = false;
    s_status.state = FIRMWARE_OTA_STATE_RUNNING;
    reset_transfer_status_locked();
    copy_text(s_status.url, sizeof(s_status.url), "upload");
    copy_text(s_status.artifact_url, sizeof(s_status.artifact_url), "upload");
    s_status.last_error[0] = '\0';
    portEXIT_CRITICAL(&s_ota_lock);

    next_partition = esp_ota_get_next_update_partition(NULL);
    if (!next_partition) {
        set_last_error_text("NEXT_PARTITION_NOT_FOUND");
        set_state(FIRMWARE_OTA_STATE_FAILED, false, false);
        return ESP_ERR_NOT_FOUND;
    }

    err = esp_ota_begin(next_partition, OTA_SIZE_UNKNOWN, &s_upload_handle);
    if (err != ESP_OK) {
        set_last_error_err(err);
        set_state(FIRMWARE_OTA_STATE_FAILED, false, false);
        return err;
    }

    portENTER_CRITICAL(&s_ota_lock);
    s_upload_active = true;
    s_upload_partition = next_partition;
    portEXIT_CRITICAL(&s_ota_lock);

    refresh_partition_labels();
    ESP_LOGI(TAG, "Upload OTA iniciado para particao %s", next_partition->label);
    return ESP_OK;
}

esp_err_t firmware_ota_upload_write(const uint8_t *data, size_t data_len)
{
    esp_err_t err = ESP_OK;

    if (!data || data_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_ota_lock);
    bool upload_active = s_upload_active;
    portEXIT_CRITICAL(&s_ota_lock);
    if (!upload_active) {
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_ota_write(s_upload_handle, data, data_len);
    if (err != ESP_OK) {
        set_last_error_err(err);
        return err;
    }

    portENTER_CRITICAL(&s_ota_lock);
    size_t bytes_written = s_status.bytes_written + data_len;
    s_status.bytes_written = bytes_written;
    portEXIT_CRITICAL(&s_ota_lock);

    return ESP_OK;
}

esp_err_t firmware_ota_upload_finalize(void)
{
    esp_err_t err = ESP_OK;

    portENTER_CRITICAL(&s_ota_lock);
    bool upload_active = s_upload_active;
    const esp_partition_t *upload_partition = s_upload_partition;
    s_upload_active = false;
    s_upload_partition = NULL;
    portEXIT_CRITICAL(&s_ota_lock);

    if (!upload_active || !upload_partition) {
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_ota_end(s_upload_handle);
    if (err != ESP_OK) {
        set_last_error_err(err);
        set_state(FIRMWARE_OTA_STATE_FAILED, false, false);
        return err;
    }

    err = esp_ota_set_boot_partition(upload_partition);
    if (err != ESP_OK) {
        set_last_error_err(err);
        set_state(FIRMWARE_OTA_STATE_FAILED, false, false);
        return err;
    }

    set_last_error_text("");
    set_state(FIRMWARE_OTA_STATE_SUCCESS, false, true);
    refresh_partition_labels();
    ESP_LOGW(TAG, "Upload OTA concluido com sucesso para %s", upload_partition->label);
    return ESP_OK;
}

void firmware_ota_upload_abort(void)
{
    portENTER_CRITICAL(&s_ota_lock);
    bool upload_active = s_upload_active;
    s_upload_active = false;
    s_upload_partition = NULL;
    portEXIT_CRITICAL(&s_ota_lock);

    if (upload_active) {
        (void)esp_ota_abort(s_upload_handle);
    }

    set_state(FIRMWARE_OTA_STATE_FAILED, false, false);
    refresh_partition_labels();
}

void firmware_ota_get_status(firmware_ota_status_t *status)
{
    if (!status) {
        return;
    }

    portENTER_CRITICAL(&s_ota_lock);
    memcpy(status, &s_status, sizeof(*status));
    portEXIT_CRITICAL(&s_ota_lock);
}

const char *firmware_ota_state_name(firmware_ota_state_t state)
{
    switch (state) {
    case FIRMWARE_OTA_STATE_IDLE:
        return "idle";
    case FIRMWARE_OTA_STATE_RUNNING:
        return "running";
    case FIRMWARE_OTA_STATE_SUCCESS:
        return "success";
    case FIRMWARE_OTA_STATE_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}
