#ifndef TELE_CA_STORE_H
#define TELE_CA_STORE_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t tele_ca_store_init(void);
esp_err_t tele_ca_store_get_version(char *out_version, size_t out_size);
esp_err_t tele_ca_store_set_version(const char *version);
esp_err_t tele_ca_store_active_matches_sha256(const char *sha256_hex, bool *out_matches);
esp_err_t tele_ca_store_apply_file(const char *verified_path, const char *version);

#ifdef __cplusplus
}
#endif

#endif
