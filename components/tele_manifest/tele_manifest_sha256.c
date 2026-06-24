#include "tele_manifest_internal.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define TELE_MANIFEST_SHA256_HEX_CHARS 64

#ifndef TELE_MANIFEST_HOST_TEST
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include "mbedtls/private/sha256.h"

struct tele_manifest_sha256_ctx {
    mbedtls_sha256_context ctx;
};
#else
struct tele_manifest_sha256_ctx {
    uint8_t unused;
};
#endif

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

esp_err_t tele_manifest_sha256_hex_to_bytes(const char *hex, uint8_t out[32])
{
    if (!hex || !out || strlen(hex) != TELE_MANIFEST_SHA256_HEX_CHARS) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < TELE_MANIFEST_SHA256_BYTES; i++) {
        int high = hex_value(hex[i * 2]);
        int low = hex_value(hex[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        out[i] = (uint8_t)((high << 4) | low);
    }

    return ESP_OK;
}

esp_err_t tele_manifest_sha256_bytes_to_hex(const uint8_t bytes[32], char out[65])
{
    static const char hex_chars[] = "0123456789abcdef";

    if (!bytes || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < TELE_MANIFEST_SHA256_BYTES; i++) {
        out[i * 2] = hex_chars[(bytes[i] >> 4) & 0x0f];
        out[i * 2 + 1] = hex_chars[bytes[i] & 0x0f];
    }
    out[TELE_MANIFEST_SHA256_HEX_CHARS] = '\0';

    return ESP_OK;
}

esp_err_t tele_manifest_sha256_begin(tele_manifest_sha256_ctx_t **out_ctx)
{
    if (!out_ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_ctx = calloc(1, sizeof(tele_manifest_sha256_ctx_t));
    if (!*out_ctx) {
        return ESP_ERR_NO_MEM;
    }

#ifndef TELE_MANIFEST_HOST_TEST
    mbedtls_sha256_init(&(*out_ctx)->ctx);
    if (mbedtls_sha256_starts(&(*out_ctx)->ctx, 0) != 0) {
        tele_manifest_sha256_abort(*out_ctx);
        *out_ctx = NULL;
        return ESP_FAIL;
    }
#endif

    return ESP_OK;
}

esp_err_t tele_manifest_sha256_update(tele_manifest_sha256_ctx_t *ctx,
                                      const uint8_t *data,
                                      size_t data_len)
{
    if (!ctx || (!data && data_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

#ifndef TELE_MANIFEST_HOST_TEST
    if (mbedtls_sha256_update(&ctx->ctx, data, data_len) != 0) {
        return ESP_FAIL;
    }
#else
    (void)data;
    (void)data_len;
#endif

    return ESP_OK;
}

esp_err_t tele_manifest_sha256_finish(tele_manifest_sha256_ctx_t *ctx,
                                      uint8_t out[32])
{
    if (!ctx || !out) {
        return ESP_ERR_INVALID_ARG;
    }

#ifndef TELE_MANIFEST_HOST_TEST
    if (mbedtls_sha256_finish(&ctx->ctx, out) != 0) {
        tele_manifest_sha256_abort(ctx);
        return ESP_FAIL;
    }
    mbedtls_sha256_free(&ctx->ctx);
#else
    memset(out, 0, 32);
#endif

    free(ctx);
    return ESP_OK;
}

void tele_manifest_sha256_abort(tele_manifest_sha256_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

#ifndef TELE_MANIFEST_HOST_TEST
    mbedtls_sha256_free(&ctx->ctx);
#endif
    free(ctx);
}
