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

#define FIRMWARE_OTA_URL_BUFFER_SIZE 384
#define FIRMWARE_OTA_ERROR_BUFFER_SIZE 128
#define FIRMWARE_OTA_PARTITION_LABEL_SIZE 32
#define FIRMWARE_OTA_TASK_STACK_SIZE 10240
#define FIRMWARE_OTA_TASK_PRIORITY 5

static const char *TAG = "firmware-ota";

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
    copy_text(s_status.url, sizeof(s_status.url), url);
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
    copy_text(s_status.url, sizeof(s_status.url), "upload");
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