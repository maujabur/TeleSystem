#ifndef TELE_MANIFEST_INTERNAL_H
#define TELE_MANIFEST_INTERNAL_H

#include "tele_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TELE_MANIFEST_SHA256_BYTES 32

typedef struct tele_manifest_sha256_ctx tele_manifest_sha256_ctx_t;

typedef esp_err_t (*tele_manifest_download_chunk_cb_t)(const uint8_t *data,
                                                       size_t data_len,
                                                       size_t received,
                                                       size_t total,
                                                       void *ctx);

esp_err_t tele_manifest_http_get_text(const char *url,
                                      char *out_text,
                                      size_t out_size,
                                      int *out_status);

esp_err_t tele_manifest_http_download(const char *url,
                                      size_t expected_size,
                                      tele_manifest_download_chunk_cb_t on_chunk,
                                      void *ctx,
                                      int *out_status,
                                      size_t *out_received);

esp_err_t tele_manifest_parse_json(const char *text,
                                   const tele_manifest_request_t *request,
                                   tele_manifest_artifact_t *out_artifact);

esp_err_t tele_manifest_sha256_hex_to_bytes(const char *hex, uint8_t out[32]);
esp_err_t tele_manifest_sha256_bytes_to_hex(const uint8_t bytes[32], char out[65]);
esp_err_t tele_manifest_sha256_begin(tele_manifest_sha256_ctx_t **out_ctx);
esp_err_t tele_manifest_sha256_update(tele_manifest_sha256_ctx_t *ctx,
                                      const uint8_t *data,
                                      size_t data_len);
esp_err_t tele_manifest_sha256_finish(tele_manifest_sha256_ctx_t *ctx,
                                      uint8_t out[32]);
void tele_manifest_sha256_abort(tele_manifest_sha256_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif
