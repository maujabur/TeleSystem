#ifndef ACR_CONFIG_STORE_H
#define ACR_CONFIG_STORE_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define ACR_REGION_BUFFER_SIZE 64
#define ACR_CONTAINER_ID_BUFFER_SIZE 64
#define ACR_TOKEN_BUFFER_SIZE 1100
#define ACR_CERT_BUFFER_SIZE 4096
#define ACR_UPLOAD_PREFIX_BUFFER_SIZE 32

typedef struct {
    char region[ACR_REGION_BUFFER_SIZE];
    char container_id[ACR_CONTAINER_ID_BUFFER_SIZE];
    char bearer_token[ACR_TOKEN_BUFFER_SIZE];
    char root_cert_pem[ACR_CERT_BUFFER_SIZE];
    const char *active_root_cert_pem;
} acr_config_t;

typedef struct {
    char region[ACR_REGION_BUFFER_SIZE];
    char container_id[ACR_CONTAINER_ID_BUFFER_SIZE];
    char upload_prefix[ACR_UPLOAD_PREFIX_BUFFER_SIZE];
    bool token_configured;
    bool root_cert_configured;
} acr_config_public_info_t;

esp_err_t acr_config_store_load(acr_config_t *config);
esp_err_t acr_config_store_get_public_info(acr_config_public_info_t *info);
esp_err_t acr_config_store_load_upload_prefix(char *out, size_t out_size);
esp_err_t acr_config_store_save_region(const char *region);
esp_err_t acr_config_store_save_container_id(const char *container_id);
esp_err_t acr_config_store_save_bearer_token(const char *token);
esp_err_t acr_config_store_save_upload_prefix(const char *prefix);
esp_err_t acr_config_store_save_root_cert(const char *pem);

#endif
