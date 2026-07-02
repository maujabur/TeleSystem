#include "tele_ca_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_spiffs.h"

#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include "mbedtls/private/sha256.h"

#define TELE_CA_STORE_SHA256_BYTES 32
#define TELE_CA_STORE_SHA256_HEX_CHARS 64

static const char *TAG = "tele_ca_store";

static uint8_t *s_active_bundle;
static size_t s_active_bundle_size;
static bool s_spiffs_mounted;

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static esp_err_t sha256_hex_to_bytes(const char *hex, uint8_t out[TELE_CA_STORE_SHA256_BYTES])
{
    if (!hex || !out || strlen(hex) != TELE_CA_STORE_SHA256_HEX_CHARS) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < TELE_CA_STORE_SHA256_BYTES; i++) {
        int high = hex_value(hex[i * 2]);
        int low = hex_value(hex[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        out[i] = (uint8_t)((high << 4) | low);
    }

    return ESP_OK;
}

static esp_err_t build_path(char *out, size_t out_size, const char *filename)
{
    const char *base_path = CONFIG_TELE_CA_STORE_BASE_PATH;

    if (!out || out_size == 0 || !filename) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t base_len = strlen(base_path);
    size_t filename_len = strlen(filename);
    size_t total_len = base_len + 1 + filename_len;
    if (total_len >= out_size) {
        ESP_LOGE(TAG, "Path too long for %s", filename);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out, base_path, base_len);
    out[base_len] = '/';
    memcpy(out + base_len + 1, filename, filename_len + 1);
    return ESP_OK;
}

static esp_err_t build_suffixed_path(char *out, size_t out_size,
                                     const char *path, const char *suffix)
{
    if (!out || out_size == 0 || !path || !suffix) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    size_t total_len = path_len + suffix_len;
    if (total_len >= out_size) {
        ESP_LOGE(TAG, "Path too long for suffix %s", suffix);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out, path, path_len);
    memcpy(out + path_len, suffix, suffix_len + 1);
    return ESP_OK;
}

static esp_err_t build_bundle_paths(char *active_path, size_t active_size,
                                    char *temp_path, size_t temp_size,
                                    char *backup_path, size_t backup_size)
{
    ESP_RETURN_ON_ERROR(build_path(active_path, active_size, CONFIG_TELE_CA_STORE_BUNDLE_FILENAME),
                        TAG, "Failed to build CA bundle path");
    ESP_RETURN_ON_ERROR(build_suffixed_path(temp_path, temp_size, active_path, ".tmp"),
                        TAG, "Failed to build CA bundle temp path");
    ESP_RETURN_ON_ERROR(build_suffixed_path(backup_path, backup_size, active_path, ".bak"),
                        TAG, "Failed to build CA bundle backup path");
    return ESP_OK;
}

static esp_err_t build_version_path(char *path, size_t path_size)
{
    return build_path(path, path_size, CONFIG_TELE_CA_STORE_VERSION_FILENAME);
}

static esp_err_t build_version_temp_path(char *path, size_t path_size)
{
    return build_path(path, path_size, CONFIG_TELE_CA_STORE_VERSION_FILENAME ".tmp");
}

static esp_err_t build_version_backup_path(char *path, size_t path_size)
{
    return build_path(path, path_size, CONFIG_TELE_CA_STORE_VERSION_FILENAME ".bak");
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static esp_err_t mount_spiffs(void)
{
    if (s_spiffs_mounted) {
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = CONFIG_TELE_CA_STORE_BASE_PATH,
        .partition_label = CONFIG_TELE_CA_STORE_STORAGE_LABEL,
        .max_files = 5,
        .format_if_mount_failed = true,
    };

    ESP_RETURN_ON_ERROR(esp_vfs_spiffs_register(&conf), TAG, "Failed to mount CA SPIFFS");

    size_t total = 0;
    size_t used = 0;
    ESP_RETURN_ON_ERROR(esp_spiffs_info(CONFIG_TELE_CA_STORE_STORAGE_LABEL, &total, &used),
                        TAG, "Failed to inspect CA SPIFFS");

    s_spiffs_mounted = true;
    ESP_LOGI(TAG, "CA SPIFFS mounted: total=%u used=%u", (unsigned)total, (unsigned)used);
    return ESP_OK;
}

static esp_err_t read_file_to_memory(const char *path, uint8_t **out_data, size_t *out_size)
{
    if (!path || !out_data || !out_size) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_data = NULL;
    *out_size = 0;

    struct stat st;
    if (stat(path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (st.st_size <= 0 || st.st_size > CONFIG_TELE_CA_STORE_MAX_BUNDLE_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        return ESP_FAIL;
    }

    uint8_t *data = malloc((size_t)st.st_size);
    if (!data) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    size_t read_len = fread(data, 1, (size_t)st.st_size, file);
    int close_err = fclose(file);
    if (read_len != (size_t)st.st_size || close_err != 0) {
        free(data);
        return ESP_FAIL;
    }

    *out_data = data;
    *out_size = read_len;
    return ESP_OK;
}

static esp_err_t set_active_bundle(uint8_t *bundle, size_t bundle_size, const char *path)
{
    esp_err_t err = esp_crt_bundle_set(bundle, bundle_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Rejected CA bundle from %s: %s", path, esp_err_to_name(err));
        return err;
    }

    free(s_active_bundle);
    s_active_bundle = bundle;
    s_active_bundle_size = bundle_size;
    ESP_LOGI(TAG, "Activated CA bundle from %s (%u bytes)", path, (unsigned)bundle_size);
    return ESP_OK;
}

static esp_err_t activate_bundle_file(const char *path)
{
    uint8_t *bundle = NULL;
    size_t bundle_size = 0;

    esp_err_t err = read_file_to_memory(path, &bundle, &bundle_size);
    if (err != ESP_OK) {
        return err;
    }

    err = set_active_bundle(bundle, bundle_size, path);
    if (err != ESP_OK) {
        free(bundle);
    }
    return err;
}

static esp_err_t recover_interrupted_promotion(void)
{
    char active_path[128];
    char temp_path[128];
    char backup_path[128];
    ESP_RETURN_ON_ERROR(build_bundle_paths(active_path, sizeof(active_path),
                                           temp_path, sizeof(temp_path),
                                           backup_path, sizeof(backup_path)),
                        TAG, "Failed to build CA recovery paths");

    if (!file_exists(active_path) && file_exists(backup_path)) {
        if (rename(backup_path, active_path) != 0) {
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "Recovered CA bundle from backup");
    }
    if (file_exists(active_path) && file_exists(backup_path)) {
        unlink(backup_path);
    }
    if (file_exists(temp_path)) {
        unlink(temp_path);
    }

    char version_path[128];
    char version_temp_path[128];
    char version_backup_path[128];
    ESP_RETURN_ON_ERROR(build_version_path(version_path, sizeof(version_path)),
                        TAG, "Failed to build CA version path");
    ESP_RETURN_ON_ERROR(build_version_temp_path(version_temp_path, sizeof(version_temp_path)),
                        TAG, "Failed to build CA version temp path");
    ESP_RETURN_ON_ERROR(build_version_backup_path(version_backup_path, sizeof(version_backup_path)),
                        TAG, "Failed to build CA version backup path");

    if (!file_exists(version_path) && file_exists(version_backup_path)) {
        if (rename(version_backup_path, version_path) != 0) {
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "Recovered CA bundle version from backup");
    }
    if (file_exists(version_path) && file_exists(version_backup_path)) {
        unlink(version_backup_path);
    }
    if (file_exists(version_temp_path)) {
        unlink(version_temp_path);
    }

    return ESP_OK;
}

static esp_err_t promote_file(const char *temp_path, const char *active_path)
{
    char backup_path[128];
    ESP_RETURN_ON_ERROR(build_suffixed_path(backup_path, sizeof(backup_path), active_path, ".bak"),
                        TAG, "Failed to build promoted file backup path");
    unlink(backup_path);

    bool has_active = file_exists(active_path);
    if (has_active && rename(active_path, backup_path) != 0) {
        return ESP_FAIL;
    }

    if (rename(temp_path, active_path) != 0) {
        if (has_active) {
            (void)rename(backup_path, active_path);
        }
        return ESP_FAIL;
    }

    if (has_active) {
        unlink(backup_path);
    }
    return ESP_OK;
}

static esp_err_t promote_version_file(const char *temp_path, const char *version_path)
{
    char backup_path[128];
    ESP_RETURN_ON_ERROR(build_version_backup_path(backup_path, sizeof(backup_path)),
                        TAG, "Failed to build version backup path");
    unlink(backup_path);

    bool has_version = file_exists(version_path);
    if (has_version && rename(version_path, backup_path) != 0) {
        return ESP_FAIL;
    }

    if (rename(temp_path, version_path) != 0) {
        if (has_version) {
            (void)rename(backup_path, version_path);
        }
        return ESP_FAIL;
    }

    if (has_version) {
        unlink(backup_path);
    }
    return ESP_OK;
}

static esp_err_t copy_file(const char *source_path, const char *dest_path, size_t *out_size)
{
    if (!source_path || !dest_path || !out_size) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_size = 0;
    FILE *source = fopen(source_path, "rb");
    if (!source) {
        return ESP_FAIL;
    }

    FILE *dest = fopen(dest_path, "wb");
    if (!dest) {
        fclose(source);
        return ESP_FAIL;
    }

    uint8_t buffer[1024];
    esp_err_t err = ESP_OK;

    while (true) {
        size_t read_len = fread(buffer, 1, sizeof(buffer), source);
        if (read_len > 0) {
            if (*out_size > CONFIG_TELE_CA_STORE_MAX_BUNDLE_SIZE ||
                read_len > CONFIG_TELE_CA_STORE_MAX_BUNDLE_SIZE - *out_size) {
                err = ESP_ERR_INVALID_SIZE;
                break;
            }

            if (fwrite(buffer, 1, read_len, dest) != read_len) {
                err = ESP_FAIL;
                break;
            }
            *out_size += read_len;
        }

        if (read_len < sizeof(buffer)) {
            if (ferror(source)) {
                err = ESP_FAIL;
            }
            break;
        }
    }

    if (err == ESP_OK && fflush(dest) != 0) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK && fsync(fileno(dest)) != 0) {
        err = ESP_FAIL;
    }
    if (fclose(dest) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    fclose(source);
    return err;
}

esp_err_t tele_ca_store_init(void)
{
    ESP_RETURN_ON_ERROR(mount_spiffs(), TAG, "CA SPIFFS init failed");
    ESP_RETURN_ON_ERROR(recover_interrupted_promotion(), TAG, "CA recovery failed");

    char active_path[128];
    ESP_RETURN_ON_ERROR(build_path(active_path, sizeof(active_path), CONFIG_TELE_CA_STORE_BUNDLE_FILENAME),
                        TAG, "Failed to build CA bundle path");

    esp_err_t err = activate_bundle_file(active_path);
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "No stored CA bundle found; using embedded ESP-IDF bundle");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Stored CA bundle invalid; using embedded ESP-IDF bundle");
        return ESP_OK;
    }

    return ESP_OK;
}

esp_err_t tele_ca_store_get_version(char *out_version, size_t out_size)
{
    if (!out_version || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    out_version[0] = '\0';

    char version_path[128];
    ESP_RETURN_ON_ERROR(build_version_path(version_path, sizeof(version_path)),
                        TAG, "Failed to build CA version path");
    FILE *file = fopen(version_path, "r");
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }

    if (!fgets(out_version, out_size, file)) {
        fclose(file);
        return ESP_FAIL;
    }

    fclose(file);
    out_version[strcspn(out_version, "\r\n")] = '\0';
    return ESP_OK;
}

esp_err_t tele_ca_store_set_version(const char *version)
{
    if (!version || version[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char version_path[128];
    char temp_path[128];
    ESP_RETURN_ON_ERROR(build_version_path(version_path, sizeof(version_path)),
                        TAG, "Failed to build CA version path");
    ESP_RETURN_ON_ERROR(build_version_temp_path(temp_path, sizeof(temp_path)),
                        TAG, "Failed to build CA version temp path");

    FILE *file = fopen(temp_path, "w");
    if (!file) {
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    if (fprintf(file, "%s\n", version) < 0) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK && fflush(file) != 0) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK && fsync(fileno(file)) != 0) {
        err = ESP_FAIL;
    }
    if (fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }

    if (err != ESP_OK) {
        unlink(temp_path);
        return err;
    }

    err = promote_version_file(temp_path, version_path);
    if (err != ESP_OK) {
        unlink(temp_path);
    }
    return err;
}

esp_err_t tele_ca_store_active_matches_sha256(const char *sha256_hex, bool *out_matches)
{
    if (!sha256_hex || !out_matches) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_matches = false;

    uint8_t expected[TELE_CA_STORE_SHA256_BYTES] = {0};
    ESP_RETURN_ON_ERROR(sha256_hex_to_bytes(sha256_hex, expected), TAG, "Invalid SHA-256");

    if (!s_active_bundle || s_active_bundle_size == 0) {
        return ESP_OK;
    }

    uint8_t actual[TELE_CA_STORE_SHA256_BYTES] = {0};
    if (mbedtls_sha256(s_active_bundle, s_active_bundle_size, actual, 0) != 0) {
        return ESP_FAIL;
    }

    *out_matches = memcmp(expected, actual, sizeof(expected)) == 0;
    return ESP_OK;
}

esp_err_t tele_ca_store_apply_file(const char *verified_path, const char *version)
{
    if (!verified_path || verified_path[0] == '\0' || !version || version[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char active_path[128];
    char temp_path[128];
    char backup_path[128];
    ESP_RETURN_ON_ERROR(build_bundle_paths(active_path, sizeof(active_path),
                                           temp_path, sizeof(temp_path),
                                           backup_path, sizeof(backup_path)),
                        TAG, "Failed to build CA bundle paths");
    (void)backup_path;

    unlink(temp_path);

    size_t copied_size = 0;
    ESP_RETURN_ON_ERROR(copy_file(verified_path, temp_path, &copied_size),
                        TAG, "Failed to copy verified CA bundle");
    if (copied_size == 0 || copied_size > CONFIG_TELE_CA_STORE_MAX_BUNDLE_SIZE) {
        unlink(temp_path);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *new_bundle = NULL;
    size_t new_bundle_size = 0;
    esp_err_t err = read_file_to_memory(temp_path, &new_bundle, &new_bundle_size);
    if (err != ESP_OK) {
        unlink(temp_path);
        return err;
    }

    uint8_t *old_bundle = s_active_bundle;
    size_t old_bundle_size = s_active_bundle_size;
    s_active_bundle = NULL;
    s_active_bundle_size = 0;

    err = set_active_bundle(new_bundle, new_bundle_size, temp_path);
    if (err != ESP_OK) {
        s_active_bundle = old_bundle;
        s_active_bundle_size = old_bundle_size;
        free(new_bundle);
        unlink(temp_path);
        return err;
    }

    err = promote_file(temp_path, active_path);
    if (err != ESP_OK) {
        if (old_bundle) {
            esp_err_t restore_err = esp_crt_bundle_set(old_bundle, old_bundle_size);
            if (restore_err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to restore previous CA bundle: %s", esp_err_to_name(restore_err));
            }
            free(s_active_bundle);
            s_active_bundle = old_bundle;
            s_active_bundle_size = old_bundle_size;
        }
        unlink(temp_path);
        return err;
    }

    free(old_bundle);

    err = tele_ca_store_set_version(version);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CA bundle applied but version store failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "CA bundle updated to version %s", version);
    return ESP_OK;
}
