#include <assert.h>
#include <string.h>

#include "tele_manifest.h"

esp_err_t tele_manifest_parse_json(const char *text,
                                   const tele_manifest_request_t *request,
                                   tele_manifest_artifact_t *out_artifact);

static const char *valid_firmware_manifest =
    "{"
    "\"schema\":1,"
    "\"artifact_type\":\"firmware\","
    "\"channel\":\"pilot\","
    "\"version\":\"0.6.10\","
    "\"build_id\":\"2026-06-24T12:00:00Z-0.6.10\","
    "\"url\":\"https://updates.example.com/telesystem/pilot/TeleSystem.bin\","
    "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
    "\"size\":1286144,"
    "\"min_version\":\"0.6.8\","
    "\"critical\":true"
    "}";

static const char *valid_ca_manifest_with_urls =
    "{"
    "\"schema\":1,"
    "\"artifact_type\":\"ca_bundle\","
    "\"channel\":\"stable\","
    "\"version\":\"2026.06.24\","
    "\"build_id\":\"2026-06-24T12:00:00Z-ca\","
    "\"urls\":["
    "\"https://updates.example.com/ca/stable/bundle_ca.bin\","
    "\"https://mirror.example.com/ca/stable/bundle_ca.bin\""
    "],"
    "\"sha256\":\"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789\","
    "\"size\":65536,"
    "\"critical\":false"
    "}";

static void expect_invalid(const char *text, const tele_manifest_request_t *request)
{
    tele_manifest_artifact_t artifact = {0};
    assert(tele_manifest_parse_json(text, request, &artifact) != ESP_OK);
}

int main(void)
{
    tele_manifest_artifact_t artifact = {0};
    const tele_manifest_request_t firmware_request = {
        .artifact_type = "firmware",
        .channel = "pilot",
        .max_artifact_size = 2 * 1024 * 1024,
    };
    const tele_manifest_request_t ca_request = {
        .artifact_type = "ca_bundle",
        .channel = "stable",
        .max_artifact_size = 128 * 1024,
    };

    assert(tele_manifest_parse_json(valid_firmware_manifest,
                                    &firmware_request,
                                    &artifact) == ESP_OK);
    assert(artifact.schema == 1);
    assert(strcmp(artifact.artifact_type, "firmware") == 0);
    assert(strcmp(artifact.channel, "pilot") == 0);
    assert(strcmp(artifact.version, "0.6.10") == 0);
    assert(strcmp(artifact.build_id, "2026-06-24T12:00:00Z-0.6.10") == 0);
    assert(artifact.url_count == 1);
    assert(strcmp(artifact.urls[0],
                  "https://updates.example.com/telesystem/pilot/TeleSystem.bin") == 0);
    assert(strcmp(artifact.sha256_hex,
                  "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef") == 0);
    assert(artifact.size == 1286144);
    assert(strcmp(artifact.min_version, "0.6.8") == 0);
    assert(artifact.critical);

    memset(&artifact, 0, sizeof(artifact));
    assert(tele_manifest_parse_json(valid_ca_manifest_with_urls,
                                    &ca_request,
                                    &artifact) == ESP_OK);
    assert(strcmp(artifact.artifact_type, "ca_bundle") == 0);
    assert(artifact.url_count == 2);
    assert(strcmp(artifact.urls[0],
                  "https://updates.example.com/ca/stable/bundle_ca.bin") == 0);
    assert(strcmp(artifact.urls[1],
                  "https://mirror.example.com/ca/stable/bundle_ca.bin") == 0);
    assert(!artifact.critical);

    expect_invalid("{\"schema\":1,\"artifact_type\":\"ca_bundle\","
                   "\"channel\":\"pilot\",\"version\":\"0.6.10\","
                   "\"url\":\"https://updates.example.com/fw.bin\","
                   "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
                   "\"size\":1024}",
                   &firmware_request);

    expect_invalid("{\"schema\":1,\"artifact_type\":\"firmware\","
                   "\"channel\":\"stable\",\"version\":\"0.6.10\","
                   "\"url\":\"https://updates.example.com/fw.bin\","
                   "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
                   "\"size\":1024}",
                   &firmware_request);

    expect_invalid("{\"schema\":1,\"artifact_type\":\"firmware\","
                   "\"channel\":\"pilot\",\"version\":\"0.6.10\","
                   "\"url\":\"https://updates.example.com/fw.bin\","
                   "\"sha256\":\"bad\","
                   "\"size\":1024}",
                   &firmware_request);

    expect_invalid("{\"schema\":1,\"artifact_type\":\"firmware\","
                   "\"channel\":\"pilot\",\"version\":\"0.6.10\","
                   "\"url\":\"http://updates.example.com/fw.bin\","
                   "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
                   "\"size\":1024}",
                   &firmware_request);

    expect_invalid("{\"schema\":1,\"artifact_type\":\"firmware\","
                   "\"channel\":\"pilot\",\"version\":\"0.6.10\","
                   "\"url\":\"https://updates.example.com/fw.bin\","
                   "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
                   "\"size\":3145728}",
                   &firmware_request);

    expect_invalid("{\"schema\":1,\"artifact_type\":\"firmware\","
                   "\"channel\":\"pilot\",\"version\":\"0.6.10\","
                   "\"urls\":["
                   "\"https://updates.example.com/1.bin\","
                   "\"https://updates.example.com/2.bin\","
                   "\"https://updates.example.com/3.bin\","
                   "\"https://updates.example.com/4.bin\","
                   "\"https://updates.example.com/5.bin\""
                   "],"
                   "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
                   "\"size\":1024}",
                   &firmware_request);

    return 0;
}
