#include "tele_manifest.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#define TELE_MANIFEST_SHA256_HEX_CHARS 64

static bool text_empty(const char *text)
{
    return !text || text[0] == '\0';
}

static bool copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return false;
    }

    dst[0] = '\0';
    if (!src) {
        return true;
    }

    return snprintf(dst, dst_size, "%s", src) < (int)dst_size;
}

static bool is_https_url(const char *url)
{
    return url && strncmp(url, "https://", strlen("https://")) == 0 && url[strlen("https://")] != '\0';
}

static bool is_hex_char(char c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static bool sha256_hex_valid(const char *sha256_hex)
{
    if (!sha256_hex || strlen(sha256_hex) != TELE_MANIFEST_SHA256_HEX_CHARS) {
        return false;
    }

    for (size_t i = 0; i < TELE_MANIFEST_SHA256_HEX_CHARS; i++) {
        if (!is_hex_char(sha256_hex[i])) {
            return false;
        }
    }

    return true;
}

static esp_err_t copy_required_string(cJSON *root, const char *name, char *dst, size_t dst_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);

    if (!cJSON_IsString(item) || text_empty(item->valuestring)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return copy_text(dst, dst_size, item->valuestring) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t copy_optional_string(cJSON *root, const char *name, char *dst, size_t dst_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);

    if (!dst || dst_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    dst[0] = '\0';

    if (!item || cJSON_IsNull(item)) {
        return ESP_OK;
    }
    if (!cJSON_IsString(item)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return copy_text(dst, dst_size, item->valuestring) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t append_url(tele_manifest_artifact_t *artifact, const char *url)
{
    if (!is_https_url(url)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (artifact->url_count >= TELE_MANIFEST_MAX_URLS) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!copy_text(artifact->urls[artifact->url_count],
                   sizeof(artifact->urls[artifact->url_count]),
                   url)) {
        return ESP_ERR_INVALID_SIZE;
    }
    artifact->url_count++;
    return ESP_OK;
}

static esp_err_t parse_urls(cJSON *root, tele_manifest_artifact_t *artifact)
{
    cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "url");
    cJSON *urls = cJSON_GetObjectItemCaseSensitive(root, "urls");

    if (url) {
        if (!cJSON_IsString(url) || text_empty(url->valuestring)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        esp_err_t err = append_url(artifact, url->valuestring);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (urls) {
        if (!cJSON_IsArray(urls)) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        cJSON *item = NULL;
        cJSON_ArrayForEach(item, urls) {
            if (!cJSON_IsString(item) || text_empty(item->valuestring)) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            esp_err_t err = append_url(artifact, item->valuestring);
            if (err != ESP_OK) {
                return err;
            }
        }
    }

    return artifact->url_count > 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t tele_manifest_parse_json(const char *text,
                                   const tele_manifest_request_t *request,
                                   tele_manifest_artifact_t *out_artifact)
{
    if (text_empty(text) || !request || !out_artifact || text_empty(request->artifact_type)) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(text);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t err = ESP_OK;
    tele_manifest_artifact_t artifact = {0};

    if (!cJSON_IsObject(root)) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    if (!cJSON_IsNumber(schema) || schema->valueint != 1) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    artifact.schema = schema->valueint;

    err = copy_required_string(root,
                               "artifact_type",
                               artifact.artifact_type,
                               sizeof(artifact.artifact_type));
    if (err != ESP_OK) {
        goto cleanup;
    }
    if (strcmp(artifact.artifact_type, request->artifact_type) != 0) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    err = copy_required_string(root, "version", artifact.version, sizeof(artifact.version));
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = copy_optional_string(root, "channel", artifact.channel, sizeof(artifact.channel));
    if (err != ESP_OK) {
        goto cleanup;
    }
    if (!text_empty(request->channel) &&
        !text_empty(artifact.channel) &&
        strcmp(artifact.channel, request->channel) != 0) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    err = copy_optional_string(root, "build_id", artifact.build_id, sizeof(artifact.build_id));
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = parse_urls(root, &artifact);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = copy_required_string(root, "sha256", artifact.sha256_hex, sizeof(artifact.sha256_hex));
    if (err != ESP_OK) {
        goto cleanup;
    }
    if (!sha256_hex_valid(artifact.sha256_hex)) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
    if (!cJSON_IsNumber(size) || size->valuedouble <= 0) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    artifact.size = (size_t)size->valuedouble;
    if (request->max_artifact_size > 0 && artifact.size > request->max_artifact_size) {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    err = copy_optional_string(root, "min_version", artifact.min_version, sizeof(artifact.min_version));
    if (err != ESP_OK) {
        goto cleanup;
    }

    cJSON *critical = cJSON_GetObjectItemCaseSensitive(root, "critical");
    artifact.critical = cJSON_IsBool(critical) && cJSON_IsTrue(critical);

    *out_artifact = artifact;

cleanup:
    cJSON_Delete(root);
    return err;
}
