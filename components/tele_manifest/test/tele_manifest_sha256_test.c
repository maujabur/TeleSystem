#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "tele_manifest_internal.h"

int main(void)
{
    uint8_t bytes[32] = {0};
    char hex[65] = {0};

    assert(tele_manifest_sha256_hex_to_bytes(
               "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
               bytes) == ESP_OK);
    assert(bytes[0] == 0x01);
    assert(bytes[1] == 0x23);
    assert(bytes[30] == 0xcd);
    assert(bytes[31] == 0xef);

    assert(tele_manifest_sha256_bytes_to_hex(bytes, hex) == ESP_OK);
    assert(strcmp(hex,
                  "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef") == 0);

    assert(tele_manifest_sha256_hex_to_bytes("bad", bytes) == ESP_ERR_INVALID_ARG);
    assert(tele_manifest_sha256_hex_to_bytes(
               "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg",
               bytes) == ESP_ERR_INVALID_ARG);
    assert(tele_manifest_sha256_hex_to_bytes(NULL, bytes) == ESP_ERR_INVALID_ARG);
    assert(tele_manifest_sha256_hex_to_bytes(
               "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
               NULL) == ESP_ERR_INVALID_ARG);
    assert(tele_manifest_sha256_bytes_to_hex(NULL, hex) == ESP_ERR_INVALID_ARG);
    assert(tele_manifest_sha256_bytes_to_hex(bytes, NULL) == ESP_ERR_INVALID_ARG);

    return 0;
}
