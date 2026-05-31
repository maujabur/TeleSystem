#include <stdio.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

#include "storage.h"

static const char *TAG = "storage";
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

esp_err_t storage_mount(void)
{
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 4096
    };

    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl("/data",
                                                     "storage",
                                                     &mount_config,
                                                     &s_wl_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Particao FAT montada em /data");
    } else {
        ESP_LOGE(TAG,
                 "Falha ao montar particao FAT em /data sem formatacao automatica: %s",
                 esp_err_to_name(err));
    }

    return err;
}

int storage_file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

long storage_file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }

    return st.st_size;
}

void storage_log_file_info(const char *path)
{
    if (!storage_file_exists(path)) {
        ESP_LOGW(TAG, "Arquivo nao encontrado: %s", path);
        return;
    }

    long size = storage_file_size(path);
    ESP_LOGI(TAG, "Arquivo encontrado: %s (%ld bytes)", path, size);

    FILE *file = fopen(path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "Nao foi possivel abrir o arquivo");
        return;
    }

    unsigned char header[32] = {0};
    size_t bytes_read = fread(header, 1, sizeof(header), file);
    fclose(file);

    ESP_LOGD(TAG, "Primeiros %d bytes lidos: %d", (int)sizeof(header), (int)bytes_read);
}
