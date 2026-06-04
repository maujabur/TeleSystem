#include <inttypes.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"

#include "cJSON.h"

#include "acr_client.h"
#include "acr_analysis_control.h"
#include "acr_parser.h"
#include "mqtt_presence.h"
#include "vbat_monitor.h"
#include "wav_writer.h"
#include "web_portal.h"
#include "wifi_manager.h"

static const char *TAG = "acr-client";

ESP_EVENT_DEFINE_BASE(ACR_CLIENT_EVENT);

#define HTTP_TIMEOUT_MS 15000
#define HTTP_CLIENT_BUFFER_SIZE 4096
#define HTTP_CLIENT_TX_BUFFER_SIZE 8192
#define HTTP_RESPONSE_BUFFER_SIZE (16 * 1024)
#define HTTP_UPLOAD_CHUNK_SIZE 16384
#define UPLOAD_NAME_PREFIX_FALLBACK "acr"

#ifndef CONFIG_ACR_POLL_MAX_ATTEMPTS
#define CONFIG_ACR_POLL_MAX_ATTEMPTS 10
#endif

#ifndef CONFIG_ACR_POLL_DELAY_MS
#define CONFIG_ACR_POLL_DELAY_MS 2000
#endif

#ifndef CONFIG_APP_ACR_LOG_HEAP_SNAPSHOT
#define CONFIG_APP_ACR_LOG_HEAP_SNAPSHOT 0
#endif

#if CONFIG_APP_ACR_LOG_HEAP_SNAPSHOT
#include "esp_heap_caps.h"
#endif

static char s_http_response_buffer[HTTP_RESPONSE_BUFFER_SIZE];
static size_t s_http_response_len = 0;

static void post_acr_event_with_file(acr_client_event_id_t event_id, const char *uploaded_name)
{
    const char *event_data = uploaded_name && uploaded_name[0] != '\0' ? uploaded_name : NULL;
    size_t event_data_size = event_data ? strlen(event_data) + 1 : 0;
    esp_err_t err = esp_event_post(ACR_CLIENT_EVENT,
                                   event_id,
                                   event_data,
                                   event_data_size,
                                   0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Nao foi possivel publicar evento ACR %d: %s",
                 (int)event_id,
                 esp_err_to_name(err));
    }
}

static void post_acr_event(acr_client_event_id_t event_id)
{
    post_acr_event_with_file(event_id, NULL);
}

static void reset_http_buffer(void)
{
    memset(s_http_response_buffer, 0, sizeof(s_http_response_buffer));
    s_http_response_len = 0;
}

static void log_heap_snapshot(const char *label)
{
#if CONFIG_APP_ACR_LOG_HEAP_SNAPSHOT
    ESP_LOGI(TAG,
             "Heap %s: livre=%u min_livre=%u maior_bloco_8bit=%u",
             label,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#else
    (void)label;
#endif
}

static void acr_suspend_local_network_services(bool *portal_was_running)
{
    esp_err_t err = ESP_OK;

    if (portal_was_running) {
        *portal_was_running = web_portal_is_running();
    }

    err = mqtt_presence_suspend_for_acr();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Nao foi possivel suspender MQTT antes do ACR: %s", esp_err_to_name(err));
    }

    if (portal_was_running && *portal_was_running) {
        err = web_portal_stop();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Nao foi possivel parar portal web antes do ACR: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Portal web suspenso durante ciclo ACR");
        }
    }

    vTaskDelay(pdMS_TO_TICKS(250));
}

static void acr_resume_local_network_services(bool portal_was_running)
{
    esp_err_t err = ESP_OK;

    if (portal_was_running) {
        wifi_manager_status_t wifi_status = {0};
        err = wifi_manager_get_status(&wifi_status);
        if (err == ESP_OK) {
            if (wifi_status.state == WIFI_MANAGER_STATE_PROVISIONING_AP) {
                err = web_portal_start(true);
            } else if (wifi_status.state == WIFI_MANAGER_STATE_STA_CONNECTED) {
                err = web_portal_start(false);
            }
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Nao foi possivel retomar portal web apos ACR: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGW(TAG, "Nao foi possivel consultar Wi-Fi para retomar portal web: %s", esp_err_to_name(err));
        }
    }

    err = mqtt_presence_resume_after_acr();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Nao foi possivel retomar MQTT apos ACR: %s", esp_err_to_name(err));
    }
}

static void trim_trailing_whitespace(char *text)
{
    size_t len = 0;

    if (!text) {
        return;
    }

    len = strlen(text);
    while (len > 0) {
        char c = text[len - 1];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            break;
        }
        text[len - 1] = '\0';
        len--;
    }
}

static int is_upload_prefix_char_allowed(char c)
{
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '_' ||
           c == '-';
}

static void sanitize_upload_prefix(char *prefix)
{
    size_t read_index = 0;
    size_t write_index = 0;

    if (!prefix) {
        return;
    }

    while (prefix[read_index] != '\0') {
        char c = prefix[read_index++];

        if (is_upload_prefix_char_allowed(c)) {
            prefix[write_index++] = c;
        } else if (write_index > 0 && prefix[write_index - 1] != '_') {
            prefix[write_index++] = '_';
        }
    }

    while (write_index > 0 && prefix[write_index - 1] == '_') {
        write_index--;
    }
    prefix[write_index] = '\0';
}

static void load_upload_name_prefix(char *prefix, size_t prefix_size)
{
    if (!prefix || prefix_size == 0) {
        return;
    }

    memset(prefix, 0, prefix_size);

    esp_err_t err = acr_config_store_load_upload_prefix(prefix, prefix_size);
    if (err == ESP_OK) {
        trim_trailing_whitespace(prefix);
        sanitize_upload_prefix(prefix);

        if (prefix[0] != '\0') {
            ESP_LOGI(TAG, "Prefixo de upload carregado: %s", prefix);
            return;
        }
    } else {
        ESP_LOGW(TAG, "Prefixo de upload indisponivel: %s", esp_err_to_name(err));
    }

    snprintf(prefix, prefix_size, "%s", UPLOAD_NAME_PREFIX_FALLBACK);
}

static void build_unique_upload_name_with_extension(const char *extension, char *out, size_t out_size)
{
    char prefix[ACR_UPLOAD_PREFIX_BUFFER_SIZE];
    unsigned long now_ms = (unsigned long)esp_log_timestamp();
    uint32_t random_value = esp_random();

    if (!extension || extension[0] != '.') {
        extension = ".wav";
    }

    load_upload_name_prefix(prefix, sizeof(prefix));
    snprintf(out, out_size, "%s_%lu_%08lx%s", prefix, now_ms, (unsigned long)random_value, extension);
}

static void build_unique_upload_name(const char *path, char *out, size_t out_size)
{
    const char *extension = strrchr(path, '.');

    if (!extension || strchr(extension, '/') != NULL) {
        extension = ".wav";
    }

    build_unique_upload_name_with_extension(extension, out, out_size);
}

static esp_err_t extract_uploaded_file_id_from_response(char *file_id, size_t file_id_size)
{
    cJSON *root = NULL;
    cJSON *data = NULL;
    cJSON *id = NULL;

    if (!file_id || file_id_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    file_id[0] = '\0';

    root = cJSON_Parse(s_http_response_buffer);
    if (!root) {
        ESP_LOGW(TAG, "Upload ACR sem JSON valido para extrair file_id");
        return ESP_FAIL;
    }

    data = cJSON_GetObjectItemCaseSensitive(root, "data");
    id = cJSON_IsObject(data) ? cJSON_GetObjectItemCaseSensitive(data, "id") : NULL;
    if (!cJSON_IsString(id) || !id->valuestring || id->valuestring[0] == '\0') {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "Resposta de upload ACR sem data.id");
        return ESP_FAIL;
    }

    snprintf(file_id, file_id_size, "%s", id->valuestring);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t build_auth_header(const acr_config_t *config, char *auth_header, size_t auth_header_size)
{
    int auth_len = snprintf(auth_header, auth_header_size, "Bearer %s", config->bearer_token);
    if (auth_len < 0 || auth_len >= (int)auth_header_size) {
        ESP_LOGE(TAG, "Bearer token grande demais");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
            break;

        case HTTP_EVENT_ON_DATA:
            if (evt->data && evt->data_len > 0) {
                size_t space = sizeof(s_http_response_buffer) - 1 - s_http_response_len;
                size_t copy_len = evt->data_len < space ? (size_t)evt->data_len : space;
                if (copy_len > 0) {
                    memcpy(s_http_response_buffer + s_http_response_len, evt->data, copy_len);
                    s_http_response_len += copy_len;
                    s_http_response_buffer[s_http_response_len] = '\0';
                }
                if (copy_len < (size_t)evt->data_len) {
                    ESP_LOGW(TAG,
                             "Resposta HTTP truncada: recebido=%d, armazenado=%u, buffer=%u",
                             evt->data_len,
                             (unsigned)s_http_response_len,
                             (unsigned)sizeof(s_http_response_buffer));
                }
            }
            break;

        default:
            break;
    }

    return ESP_OK;
}

static esp_err_t http_client_write_all(esp_http_client_handle_t client, const void *data, size_t len)
{
    const char *buffer = (const char *)data;
    size_t written_total = 0;

    while (written_total < len) {
        int written = esp_http_client_write(client, buffer + written_total, len - written_total);
        if (written < 0) {
            ESP_LOGE(TAG, "Falha ao escrever payload HTTP");
            return ESP_FAIL;
        }
        written_total += (size_t)written;
    }

    return ESP_OK;
}

static esp_err_t http_client_read_response(esp_http_client_handle_t client)
{
    while (1) {
        int read_len = esp_http_client_read(client,
                                            s_http_response_buffer + s_http_response_len,
                                            sizeof(s_http_response_buffer) - 1 - s_http_response_len);
        if (read_len < 0) {
            ESP_LOGE(TAG, "Falha ao ler resposta HTTP");
            return ESP_FAIL;
        }

        if (read_len == 0) {
            break;
        }

        s_http_response_len += (size_t)read_len;
        s_http_response_buffer[s_http_response_len] = '\0';

        if (s_http_response_len >= sizeof(s_http_response_buffer) - 1) {
            ESP_LOGW(TAG, "Resposta HTTP truncada no buffer local");
            break;
        }
    }

    return ESP_OK;
}

static esp_err_t acr_upload_file(const acr_config_t *config,
                                 const char *path,
                                 char *uploaded_name,
                                 size_t uploaded_name_size,
                                 char *uploaded_file_id,
                                 size_t uploaded_file_id_size,
                                 acr_client_result_t *out)
{
    char url[256];
    char auth_header[1100];
    char boundary[64];
    char content_type[128];
    char multipart_header[512];
    char multipart_footer[128];
    long file_size = 0;
    esp_err_t err = ESP_FAIL;
    FILE *file = NULL;
    uint8_t *chunk = NULL;
    esp_http_client_handle_t client = NULL;
    int64_t connect_start_ms = 0;
    int64_t connect_end_ms = 0;
    int64_t write_start_ms = 0;
    int64_t write_end_ms = 0;
    int64_t response_start_ms = 0;
    int64_t response_end_ms = 0;

    snprintf(url, sizeof(url),
             "https://api-%s.acrcloud.com/api/fs-containers/%s/files",
             config->region, config->container_id);

    err = build_auth_header(config, auth_header, sizeof(auth_header));
    if (err != ESP_OK) {
        return err;
    }

    struct stat st = {0};
    file_size = stat(path, &st) == 0 ? st.st_size : -1;
    if (file_size <= 0) {
        ESP_LOGE(TAG, "Arquivo invalido para upload: %s", path);
        return ESP_FAIL;
    }

    build_unique_upload_name(path, uploaded_name, uploaded_name_size);
    snprintf(boundary, sizeof(boundary), "----esp32-acr-%08" PRIx32, esp_random());
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);

    int multipart_header_len = snprintf(
        multipart_header,
        sizeof(multipart_header),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"data_type\"\r\n\r\n"
        "audio\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: audio/wav\r\n\r\n",
        boundary,
        boundary,
        uploaded_name);
    int multipart_footer_len = snprintf(multipart_footer, sizeof(multipart_footer), "\r\n--%s--\r\n", boundary);

    if (multipart_header_len < 0 || multipart_header_len >= (int)sizeof(multipart_header) ||
        multipart_footer_len < 0 || multipart_footer_len >= (int)sizeof(multipart_footer)) {
        ESP_LOGE(TAG, "Payload multipart excedeu o buffer");
        return ESP_FAIL;
    }

    esp_http_client_config_t http_config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = HTTP_CLIENT_BUFFER_SIZE,
        .buffer_size_tx = HTTP_CLIENT_TX_BUFFER_SIZE,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    client = esp_http_client_init(&http_config);
    if (!client) {
        ESP_LOGE(TAG, "Falha ao criar cliente HTTP para upload");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", content_type);

    file = fopen(path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "Nao foi possivel abrir arquivo para upload: %s", path);
        goto cleanup;
    }

    chunk = malloc(HTTP_UPLOAD_CHUNK_SIZE);
    if (!chunk) {
        ESP_LOGE(TAG, "Sem memoria para buffer de upload (%u bytes)",
                 (unsigned)HTTP_UPLOAD_CHUNK_SIZE);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    reset_http_buffer();

    ESP_LOGI(TAG, "Enviando arquivo para ACRCloud com nome unico: %s", uploaded_name);
    post_acr_event_with_file(ACR_CLIENT_EVENT_UPLOAD_STARTED, uploaded_name);
    log_heap_snapshot("antes do upload HTTP");
    connect_start_ms = esp_log_timestamp();
    errno = 0;
    err = esp_http_client_open(client, multipart_header_len + (int)file_size + multipart_footer_len);
    connect_end_ms = esp_log_timestamp();
    if (out) {
        out->upload_connect_ms = connect_end_ms - connect_start_ms;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Falha ao abrir conexao HTTP: %s | errno=%d",
                 esp_err_to_name(err),
                 errno);
        log_heap_snapshot("apos falha ao abrir HTTP");
        goto cleanup;
    }

    write_start_ms = esp_log_timestamp();
    err = http_client_write_all(client, multipart_header, multipart_header_len);
    if (err != ESP_OK) {
        goto cleanup;
    }

    while (!feof(file)) {
        size_t bytes_read = fread(chunk, 1, HTTP_UPLOAD_CHUNK_SIZE, file);
        if (bytes_read > 0) {
            err = http_client_write_all(client, chunk, bytes_read);
            if (err != ESP_OK) {
                goto cleanup;
            }
        }

        if (ferror(file)) {
            ESP_LOGE(TAG, "Falha ao ler arquivo durante upload");
            err = ESP_FAIL;
            goto cleanup;
        }
    }

    err = http_client_write_all(client, multipart_footer, multipart_footer_len);
    write_end_ms = esp_log_timestamp();
    if (out) {
        out->upload_write_ms = write_end_ms - write_start_ms;
    }
    if (err != ESP_OK) {
        goto cleanup;
    }

    response_start_ms = esp_log_timestamp();
    if (esp_http_client_fetch_headers(client) < 0) {
        ESP_LOGE(TAG, "Falha ao obter cabecalhos da resposta do upload");
        err = ESP_FAIL;
        goto cleanup;
    }

    err = http_client_read_response(client);
    if (err != ESP_OK) {
        goto cleanup;
    }
    response_end_ms = esp_log_timestamp();
    if (out) {
        out->upload_response_ms = response_end_ms - response_start_ms;
    }

    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Status HTTP do upload: %d", status);
    ESP_LOGI(TAG,
             "Tempos HTTP upload: connect=%lldms write=%lldms response=%lldms",
             (long long)(out ? out->upload_connect_ms : connect_end_ms - connect_start_ms),
             (long long)(out ? out->upload_write_ms : write_end_ms - write_start_ms),
             (long long)(out ? out->upload_response_ms : response_end_ms - response_start_ms));
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "Resposta inesperada no upload: %s", s_http_response_buffer);
        err = ESP_FAIL;
        goto cleanup;
    }

    if (uploaded_file_id && uploaded_file_id_size > 0) {
        err = extract_uploaded_file_id_from_response(uploaded_file_id, uploaded_file_id_size);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "File ID do upload ACR: %s", uploaded_file_id);
        } else {
            ESP_LOGW(TAG, "Continuando sem file_id do upload ACR");
            err = ESP_OK;
        }
    }

    err = ESP_OK;

cleanup:
    if (file) {
        fclose(file);
    }
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    free(chunk);

    return err;
}

static esp_err_t acr_upload_pcm_wav_memory(const acr_config_t *config,
                                           const void *pcm_data,
                                           size_t pcm_size,
                                           int sample_rate_hz,
                                           int channels,
                                           int bits_per_sample,
                                           char *uploaded_name,
                                           size_t uploaded_name_size,
                                           char *uploaded_file_id,
                                           size_t uploaded_file_id_size,
                                           acr_client_result_t *out)
{
    char url[256];
    char auth_header[1100];
    char boundary[64];
    char content_type[128];
    char multipart_header[512];
    char multipart_footer[128];
    uint8_t wav_header[WAV_WRITER_HEADER_SIZE] = {0};
    esp_err_t err = ESP_FAIL;
    esp_http_client_handle_t client = NULL;
    int64_t connect_start_ms = 0;
    int64_t connect_end_ms = 0;
    int64_t write_start_ms = 0;
    int64_t write_end_ms = 0;
    int64_t response_start_ms = 0;
    int64_t response_end_ms = 0;

    if (!pcm_data || pcm_size == 0 ||
        sample_rate_hz <= 0 || channels <= 0 || bits_per_sample <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(url, sizeof(url),
             "https://api-%s.acrcloud.com/api/fs-containers/%s/files",
             config->region, config->container_id);

    err = build_auth_header(config, auth_header, sizeof(auth_header));
    if (err != ESP_OK) {
        return err;
    }

    wav_writer_config_t wav_config = {
        .sample_rate_hz = sample_rate_hz,
        .channels = channels,
        .bits_per_sample = bits_per_sample,
    };
    wav_writer_build_header(wav_header, &wav_config, (uint32_t)pcm_size);

    build_unique_upload_name_with_extension(".wav", uploaded_name, uploaded_name_size);
    snprintf(boundary, sizeof(boundary), "----esp32-acr-%08" PRIx32, esp_random());
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);

    int multipart_header_len = snprintf(
        multipart_header,
        sizeof(multipart_header),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"data_type\"\r\n\r\n"
        "audio\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: audio/wav\r\n\r\n",
        boundary,
        boundary,
        uploaded_name);
    int multipart_footer_len = snprintf(multipart_footer, sizeof(multipart_footer), "\r\n--%s--\r\n", boundary);

    if (multipart_header_len < 0 || multipart_header_len >= (int)sizeof(multipart_header) ||
        multipart_footer_len < 0 || multipart_footer_len >= (int)sizeof(multipart_footer)) {
        ESP_LOGE(TAG, "Payload multipart excedeu o buffer");
        return ESP_FAIL;
    }

    esp_http_client_config_t http_config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = HTTP_CLIENT_BUFFER_SIZE,
        .buffer_size_tx = HTTP_CLIENT_TX_BUFFER_SIZE,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    client = esp_http_client_init(&http_config);
    if (!client) {
        ESP_LOGE(TAG, "Falha ao criar cliente HTTP para upload em memoria");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", content_type);

    reset_http_buffer();

    ESP_LOGI(TAG,
             "Enviando WAV em memoria para ACRCloud: %s (%u bytes PCM)",
             uploaded_name,
             (unsigned)pcm_size);
    post_acr_event_with_file(ACR_CLIENT_EVENT_UPLOAD_STARTED, uploaded_name);
    log_heap_snapshot("antes do upload HTTP em memoria");
    connect_start_ms = esp_log_timestamp();
    errno = 0;
    err = esp_http_client_open(client,
                               multipart_header_len +
                               WAV_WRITER_HEADER_SIZE +
                               (int)pcm_size +
                               multipart_footer_len);
    connect_end_ms = esp_log_timestamp();
    if (out) {
        out->upload_connect_ms = connect_end_ms - connect_start_ms;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Falha ao abrir conexao HTTP: %s | errno=%d",
                 esp_err_to_name(err),
                 errno);
        log_heap_snapshot("apos falha ao abrir HTTP");
        goto cleanup;
    }

    write_start_ms = esp_log_timestamp();
    err = http_client_write_all(client, multipart_header, multipart_header_len);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = http_client_write_all(client, wav_header, sizeof(wav_header));
    if (err != ESP_OK) {
        goto cleanup;
    }

    const uint8_t *pcm_bytes = (const uint8_t *)pcm_data;
    size_t uploaded = 0;
    while (uploaded < pcm_size) {
        size_t chunk_size = pcm_size - uploaded;
        if (chunk_size > HTTP_UPLOAD_CHUNK_SIZE) {
            chunk_size = HTTP_UPLOAD_CHUNK_SIZE;
        }

        err = http_client_write_all(client, pcm_bytes + uploaded, chunk_size);
        if (err != ESP_OK) {
            goto cleanup;
        }
        uploaded += chunk_size;
    }

    err = http_client_write_all(client, multipart_footer, multipart_footer_len);
    write_end_ms = esp_log_timestamp();
    if (out) {
        out->upload_write_ms = write_end_ms - write_start_ms;
    }
    if (err != ESP_OK) {
        goto cleanup;
    }

    response_start_ms = esp_log_timestamp();
    if (esp_http_client_fetch_headers(client) < 0) {
        ESP_LOGE(TAG, "Falha ao obter cabecalhos da resposta do upload");
        err = ESP_FAIL;
        goto cleanup;
    }

    err = http_client_read_response(client);
    if (err != ESP_OK) {
        goto cleanup;
    }
    response_end_ms = esp_log_timestamp();
    if (out) {
        out->upload_response_ms = response_end_ms - response_start_ms;
    }

    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Status HTTP do upload: %d", status);
    ESP_LOGI(TAG,
             "Tempos HTTP upload memoria: connect=%lldms write=%lldms response=%lldms",
             (long long)(out ? out->upload_connect_ms : connect_end_ms - connect_start_ms),
             (long long)(out ? out->upload_write_ms : write_end_ms - write_start_ms),
             (long long)(out ? out->upload_response_ms : response_end_ms - response_start_ms));
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "Resposta inesperada no upload: %s", s_http_response_buffer);
        err = ESP_FAIL;
        goto cleanup;
    }

    if (uploaded_file_id && uploaded_file_id_size > 0) {
        err = extract_uploaded_file_id_from_response(uploaded_file_id, uploaded_file_id_size);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "File ID do upload ACR: %s", uploaded_file_id);
        } else {
            ESP_LOGW(TAG, "Continuando sem file_id do upload ACR");
            err = ESP_OK;
        }
    }

    err = ESP_OK;

cleanup:
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }

    return err;
}

static void normalize_prediction(const char *prediction, char *out, size_t out_size)
{
    size_t write_index = 0;

    if (!out || out_size == 0) {
        return;
    }

    out[0] = '\0';
    if (!prediction) {
        return;
    }

    for (size_t i = 0; prediction[i] != '\0' && write_index + 1 < out_size; ++i) {
        unsigned char c = (unsigned char)prediction[i];
        if (isalnum(c)) {
            out[write_index++] = (char)tolower(c);
        }
    }
    out[write_index] = '\0';
}

static bool prediction_matches_ai_values(const char *prediction)
{
    char normalized[32] = {0};
    normalize_prediction(prediction, normalized, sizeof(normalized));

    return strcmp(normalized, "ai") == 0 ||
           strcmp(normalized, "aigenerated") == 0 ||
           strcmp(normalized, "aigc") == 0 ||
           strcmp(normalized, "generated") == 0;
}

static bool acr_trigger_policy_matches(const acr_result_t *result)
{
    acr_analysis_control_config_t config = {
        .trigger_mode = ACR_TRIGGER_MODE_PREDICTION_ONLY,
        .ai_probability_threshold = 70.0,
    };
    bool prediction_match = prediction_matches_ai_values(result->prediction);
    bool probability_match = false;

    if (acr_analysis_control_get_config(&config) != ESP_OK) {
        ESP_LOGW(TAG, "Usando politica ACR padrao por falha ao carregar controle");
    }
    probability_match = result->ai_probability >= config.ai_probability_threshold;

    switch (config.trigger_mode) {
    case ACR_TRIGGER_MODE_PREDICTION_ONLY:
        return prediction_match;
    case ACR_TRIGGER_MODE_PROBABILITY_ONLY:
        return probability_match;
    case ACR_TRIGGER_MODE_PREDICTION_OR_PROBABILITY:
        return prediction_match || probability_match;
    case ACR_TRIGGER_MODE_PREDICTION_AND_PROBABILITY:
    default:
        return prediction_match && probability_match;
    }
}

static void fill_client_result(const acr_result_t *acr_result,
                               const char *uploaded_name,
                               acr_client_result_t *out)
{
    if (!out) {
        return;
    }

    out->ai_probability = acr_result->ai_probability;
    snprintf(out->prediction, sizeof(out->prediction), "%s", acr_result->prediction);
    snprintf(out->uploaded_name, sizeof(out->uploaded_name), "%s", uploaded_name);

    out->trigger = acr_trigger_policy_matches(acr_result);
    out->decision = out->trigger ? ACR_DECISION_AI : ACR_DECISION_HUMAN;
}

static esp_err_t acr_wait_for_result(const acr_config_t *config,
                                     const char *uploaded_name,
                                     const char *uploaded_file_id,
                                     acr_client_result_t *out)
{
    char url[384];
    char auth_header[1100];
    int attempt = 0;

    if (uploaded_file_id && uploaded_file_id[0] != '\0') {
        snprintf(url, sizeof(url),
                 "https://api-%s.acrcloud.com/api/fs-containers/%s/files/%s",
                 config->region, config->container_id, uploaded_file_id);
        ESP_LOGI(TAG, "Polling ACR por file_id: %s", uploaded_file_id);
    } else {
        snprintf(url, sizeof(url),
                 "https://api-%s.acrcloud.com/api/fs-containers/%s/files?search=%s&with_result=1&per_page=1",
                 config->region, config->container_id, uploaded_name);
        ESP_LOGW(TAG, "Polling ACR por search de nome por falta de file_id");
    }

    esp_err_t err = build_auth_header(config, auth_header, sizeof(auth_header));
    if (err != ESP_OK) {
        return err;
    }

    post_acr_event_with_file(ACR_CLIENT_EVENT_WAITING_RESULT, uploaded_name);

    for (attempt = 1; attempt <= CONFIG_ACR_POLL_MAX_ATTEMPTS; ++attempt) {
        acr_result_t result;

        reset_http_buffer();

        esp_http_client_config_t http_config = {
            .url = url,
            .method = HTTP_METHOD_GET,
            .timeout_ms = HTTP_TIMEOUT_MS,
            .buffer_size = HTTP_CLIENT_BUFFER_SIZE,
            .buffer_size_tx = HTTP_CLIENT_TX_BUFFER_SIZE,
            .event_handler = http_event_handler,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };

        esp_http_client_handle_t client = esp_http_client_init(&http_config);
        if (!client) {
            ESP_LOGE(TAG, "Falha ao criar cliente HTTP para consulta");
            return ESP_FAIL;
        }

        esp_http_client_set_header(client, "Accept", "application/json");
        esp_http_client_set_header(client, "Authorization", auth_header);

        ESP_LOGI(TAG, "Consultando resultado da ACRCloud (%d/%d) para %s",
                 attempt,
                 CONFIG_ACR_POLL_MAX_ATTEMPTS,
                 uploaded_name);
        err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err != ESP_OK) {
            if (err == ESP_ERR_HTTP_EAGAIN || err == ESP_ERR_TIMEOUT) {
                ESP_LOGW(TAG,
                         "Consulta ACRCloud sem resposta pronta (%d/%d): %s",
                         attempt,
                         CONFIG_ACR_POLL_MAX_ATTEMPTS,
                         esp_err_to_name(err));
            } else {
                ESP_LOGE(TAG, "Falha na consulta da ACRCloud: %s", esp_err_to_name(err));
                return err;
            }
        } else if (status < 200 || status >= 300) {
            ESP_LOGW(TAG, "Resposta HTTP inesperada da ACRCloud: %d", status);
            return ESP_FAIL;
        } else if (!acr_parser_extract_matching_file_result(s_http_response_buffer, uploaded_name, &result)) {
            ESP_LOGW(TAG, "Arquivo %s ainda nao apareceu na consulta", uploaded_name);
        } else if (result.state == 0) {
            ESP_LOGI(TAG, "Arquivo %s ainda esta em processamento", uploaded_name);
        } else if (result.state == 1) {
            acr_client_result_t local_result = {0};
            acr_client_result_t *effective_result = out ? out : &local_result;

            ESP_LOGI(TAG, "Resultado confirmado para o arquivo enviado: %s", uploaded_name);
            acr_parser_log_result_summary(&result);
            fill_client_result(&result, uploaded_name, effective_result);
            if (effective_result->trigger) {
                post_acr_event_with_file(ACR_CLIENT_EVENT_RESULT_AI, uploaded_name);
            } else {
                post_acr_event_with_file(ACR_CLIENT_EVENT_RESULT_HUMAN, uploaded_name);
            }
            ESP_LOGI(TAG,
                     "Decisao ACR: %s | trigger=%s | prediction=%s | probability=%.2f%%",
                     effective_result->decision == ACR_DECISION_AI ? "IA" : "humano",
                     effective_result->trigger ? "sim" : "nao",
                     result.prediction,
                     result.ai_probability);
            return ESP_OK;
        } else if (result.state == -1) {
            ESP_LOGW(TAG, "Arquivo %s processado, mas sem resultados reconhecidos", uploaded_name);
            return ESP_FAIL;
        } else {
            ESP_LOGW(TAG, "Arquivo %s retornou estado de erro: %d", uploaded_name, result.state);
            return ESP_FAIL;
        }

        if (attempt < CONFIG_ACR_POLL_MAX_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_ACR_POLL_DELAY_MS));
        }
    }

    ESP_LOGE(TAG, "Timeout aguardando o resultado do arquivo %s", uploaded_name);
    return ESP_ERR_TIMEOUT;
}

esp_err_t acr_client_submit_and_wait_result(const acr_config_t *config,
                                            const char *path,
                                            acr_client_result_t *out)
{
    char uploaded_name[128];
    char uploaded_file_id[64];
    esp_err_t err = ESP_OK;
    esp_err_t restore_err = ESP_OK;
    bool portal_was_running = false;
    int64_t upload_start_ms = 0;
    int64_t upload_end_ms = 0;
    int64_t wait_start_ms = 0;
    int64_t wait_end_ms = 0;

    if (!config || !path) {
        return ESP_ERR_INVALID_ARG;
    }
    uploaded_file_id[0] = '\0';
    if (out) {
        memset(out, 0, sizeof(*out));
        out->decision = ACR_DECISION_UNKNOWN;
    }

    vbat_monitor_maybe_measure(VBAT_MONITOR_MOMENT_BEFORE_WIFI_PS_EXIT);
    acr_suspend_local_network_services(&portal_was_running);

    err = wifi_manager_set_high_throughput_mode(true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Nao foi possivel ativar modo high throughput Wi-Fi: %s", esp_err_to_name(err));
    }

    upload_start_ms = esp_log_timestamp();
    err = acr_upload_file(config, path, uploaded_name, sizeof(uploaded_name), uploaded_file_id, sizeof(uploaded_file_id), out);
    upload_end_ms = esp_log_timestamp();
    if (out) {
        snprintf(out->uploaded_name, sizeof(out->uploaded_name), "%s", uploaded_name);
        out->upload_ms = upload_end_ms - upload_start_ms;
    }
    if (err != ESP_OK) {
        post_acr_event(ACR_CLIENT_EVENT_FAILED);
        goto cleanup;
    }

    wait_start_ms = esp_log_timestamp();
    err = acr_wait_for_result(config, uploaded_name, uploaded_file_id, out);
    wait_end_ms = esp_log_timestamp();
    if (out) {
        out->response_wait_ms = wait_end_ms - wait_start_ms;
    }
    if (err != ESP_OK) {
        post_acr_event_with_file(ACR_CLIENT_EVENT_FAILED, uploaded_name);
    }
    vbat_monitor_maybe_measure(VBAT_MONITOR_MOMENT_AFTER_ACR_TX);

cleanup:
    restore_err = wifi_manager_set_high_throughput_mode(false);
    if (restore_err != ESP_OK) {
        ESP_LOGW(TAG, "Nao foi possivel restaurar power save Wi-Fi: %s", esp_err_to_name(restore_err));
    }
    acr_resume_local_network_services(portal_was_running);

    return err;
}

esp_err_t acr_client_submit_pcm_wav_and_wait_result(const acr_config_t *config,
                                                    const void *pcm_data,
                                                    size_t pcm_size,
                                                    int sample_rate_hz,
                                                    int channels,
                                                    int bits_per_sample,
                                                    acr_client_result_t *out)
{
    char uploaded_name[128];
    char uploaded_file_id[64];
    esp_err_t err = ESP_OK;
    esp_err_t restore_err = ESP_OK;
    bool portal_was_running = false;
    int64_t upload_start_ms = 0;
    int64_t upload_end_ms = 0;
    int64_t wait_start_ms = 0;
    int64_t wait_end_ms = 0;

    if (!config || !pcm_data || pcm_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uploaded_file_id[0] = '\0';
    if (out) {
        memset(out, 0, sizeof(*out));
        out->decision = ACR_DECISION_UNKNOWN;
    }

    vbat_monitor_maybe_measure(VBAT_MONITOR_MOMENT_BEFORE_WIFI_PS_EXIT);
    acr_suspend_local_network_services(&portal_was_running);

    err = wifi_manager_set_high_throughput_mode(true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Nao foi possivel ativar modo high throughput Wi-Fi: %s", esp_err_to_name(err));
    }

    upload_start_ms = esp_log_timestamp();
    err = acr_upload_pcm_wav_memory(config,
                                    pcm_data,
                                    pcm_size,
                                    sample_rate_hz,
                                    channels,
                                    bits_per_sample,
                                    uploaded_name,
                                    sizeof(uploaded_name),
                                    uploaded_file_id,
                                    sizeof(uploaded_file_id),
                                    out);
    upload_end_ms = esp_log_timestamp();
    if (out) {
        snprintf(out->uploaded_name, sizeof(out->uploaded_name), "%s", uploaded_name);
        out->upload_ms = upload_end_ms - upload_start_ms;
    }
    if (err != ESP_OK) {
        post_acr_event(ACR_CLIENT_EVENT_FAILED);
        goto cleanup;
    }

    wait_start_ms = esp_log_timestamp();
    err = acr_wait_for_result(config, uploaded_name, uploaded_file_id, out);
    wait_end_ms = esp_log_timestamp();
    if (out) {
        out->response_wait_ms = wait_end_ms - wait_start_ms;
    }
    if (err != ESP_OK) {
        post_acr_event_with_file(ACR_CLIENT_EVENT_FAILED, uploaded_name);
    }
    vbat_monitor_maybe_measure(VBAT_MONITOR_MOMENT_AFTER_ACR_TX);

cleanup:
    restore_err = wifi_manager_set_high_throughput_mode(false);
    if (restore_err != ESP_OK) {
        ESP_LOGW(TAG, "Nao foi possivel restaurar power save Wi-Fi: %s", esp_err_to_name(restore_err));
    }
    acr_resume_local_network_services(portal_was_running);

    return err;
}

esp_err_t acr_client_submit_and_wait(const acr_config_t *config, const char *path)
{
    return acr_client_submit_and_wait_result(config, path, NULL);
}
