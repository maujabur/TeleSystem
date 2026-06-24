# Manifest Updates For CA And OTA Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build one manifest-based update foundation that updates runtime CA bundles and firmware OTA artifacts without requiring temporary firmware files.

**Architecture:** Introduce a generic manifest updater with two delivery modes: verified file apply for CA bundles and verified streaming apply for firmware OTA. The manifest layer owns HTTPS fetch, JSON validation, version/channel policy, size limits, SHA-256, mirrors, progress, and result reporting; domain adapters own semantic apply, storage, rollback/reboot decisions, and product policy.

**Tech Stack:** ESP-IDF >=5.3, `esp_http_client`, `esp_crt_bundle`, `esp_ota_ops`, `esp_https_ota` only where still useful, `cJSON`, `mbedtls` SHA-256 public APIs where available, FreeRTOS tasks, existing `tele_system`, `tele_config`, `tele_status`, `tele_commands`, `tele_mqtt`, and `tele_portal_ota`.

---

## Scope

This plan intentionally does not preserve the current public API shape if a cleaner API is better. The project is in active development, so rename, split, or replace functions when it improves the long-term component boundary.

The final system must support:

- CA bundle updates by manifest in this TeleSystem project.
- Firmware OTA updates by manifest in this TeleSystem project.
- Firmware OTA artifact download in streaming mode directly into the OTA partition.
- Shared manifest parsing and transport validation for both artifact types.
- Domain-specific adapters for `artifact_type = "ca_bundle"` and `artifact_type = "firmware"`.
- Manual command-driven update first; automatic scheduling can be added after the core path is proven.

The first implementation should not add signed manifests. Keep the schema shaped so signatures and key rotation can be added later without rewriting the updater.

## Existing Context

Current local OTA code:

- `components/tele_system/firmware_ota.c` has URL OTA through `esp_https_ota()`.
- `components/tele_system/firmware_ota.c` has upload OTA in streaming form through `firmware_ota_upload_begin()`, `firmware_ota_upload_write()`, `firmware_ota_upload_finalize()`, and `firmware_ota_upload_abort()`.
- `components/tele_portal_ota/tele_portal_ota.c` already streams uploaded HTTP body chunks into those callbacks.
- `partitions.csv` has two OTA slots and no SPIFFS/FAT staging partition large enough for a full firmware image.
- `docs/plano_ota_remoto_https.md` already describes manifest-based remote OTA and explains why firmware should stream directly into the OTA partition.

External reference repository:

- `maujabur/mozilla_ca_spiffs_updater`
- Useful components: `manifest_file_updater`, `ca_manager`, `ca_manifest_updater`
- Reuse the architecture and field vocabulary, but do not copy the file-only updater unchanged.

## Target File Structure

Create a reusable manifest foundation:

```text
components/tele_manifest/
  CMakeLists.txt
  Kconfig
  idf_component.yml
  include/tele_manifest.h
  tele_manifest.c
  tele_manifest_http.c
  tele_manifest_json.c
  tele_manifest_sha256.c
```

Responsibilities:

- Fetch small manifests over HTTPS.
- Parse and validate schema 1 JSON.
- Normalize `url` and `urls`.
- Validate HTTPS URLs, expected artifact type, channel, version, size, and SHA-256.
- Download artifacts with size and SHA-256 verification.
- Support file apply and streaming apply.
- Return structured result details.

Add CA adapter:

```text
components/tele_ca_store/
  CMakeLists.txt
  Kconfig
  idf_component.yml
  include/tele_ca_store.h
  tele_ca_store.c

components/tele_ca_updater/
  CMakeLists.txt
  Kconfig
  idf_component.yml
  include/tele_ca_updater.h
  tele_ca_updater.c
```

Responsibilities:

- `tele_ca_store` owns local CA bundle storage, activation through `esp_crt_bundle_set()`, semantic validation, safe promotion, and stored version.
- `tele_ca_updater` adapts manifest artifact `ca_bundle` to `tele_ca_store`.

Evolve firmware OTA:

```text
components/tele_system/
  include/firmware_ota.h
  firmware_ota.c
  Kconfig
  CMakeLists.txt
  idf_component.yml
```

Responsibilities:

- Keep manual upload OTA streaming.
- Add manifest OTA streaming.
- Track progress and target metadata.
- Set boot partition only after OTA image, size, and SHA-256 verification succeed.

Expose command/control:

```text
main/main.c
components/tele_commands/tele_commands.c
components/tele_mqtt/tele_mqtt.c
components/tele_portal_ota/tele_portal_ota.c
docs/plano_ota_remoto_https.md
docs/arquitetura_index.md
```

Responsibilities:

- Wire adapters into the app.
- Register commands for check/apply.
- Optionally expose manifest OTA status in the portal.
- Update docs after behavior exists.

## Manifest Schema

Use one manifest per artifact. Initial schema:

```json
{
  "schema": 1,
  "artifact_type": "firmware",
  "channel": "pilot",
  "version": "0.6.10",
  "build_id": "2026-06-24T12:00:00Z-0.6.10",
  "url": "https://updates.example.com/telesystem/pilot/TeleSystem.bin",
  "urls": [
    "https://updates.example.com/telesystem/pilot/TeleSystem.bin",
    "https://mirror.example.com/telesystem/pilot/TeleSystem.bin"
  ],
  "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  "size": 1286144,
  "min_version": "0.6.8",
  "critical": false,
  "notes": "Corrige OTA remoto"
}
```

Rules:

- `schema` must be `1`.
- `artifact_type` must match the caller expectation.
- `channel` must match configured channel when configured.
- `version`, `url` or `urls`, `sha256`, and `size` are required.
- `url` and `urls` are both accepted; internally normalize to a URL list.
- All artifact URLs must be HTTPS in the first implementation.
- `sha256` must be exactly 64 hex characters.
- `size` must be greater than zero and less than the caller max.
- `min_version` is optional; when present, the domain adapter decides comparison semantics.
- `notes` is optional and informational only.

## Version Policy

Add normalized firmware version fields before remote OTA is treated as production-ready:

```c
#define APP_VERSION_SEMVER "0.6.10"
#define APP_VERSION_LABEL "remote manifest ota"
#define APP_BUILD_ID "2026-06-24T12:00:00Z-a6b102a"
#define APP_VERSION_STRING APP_VERSION_SEMVER " " APP_VERSION_LABEL
```

CA bundle versions may stay string/date based. Firmware versions should use semver comparison. The generic manifest updater should accept a domain callback:

```c
typedef enum {
    TELE_MANIFEST_VERSION_REJECT = 0,
    TELE_MANIFEST_VERSION_SKIP_CURRENT,
    TELE_MANIFEST_VERSION_APPLY,
} tele_manifest_version_decision_t;

typedef tele_manifest_version_decision_t (*tele_manifest_version_cb_t)(
    const tele_manifest_artifact_t *artifact,
    void *ctx);
```

## Task 1: Create Manifest Component Skeleton

**Files:**

- Create: `components/tele_manifest/CMakeLists.txt`
- Create: `components/tele_manifest/Kconfig`
- Create: `components/tele_manifest/idf_component.yml`
- Create: `components/tele_manifest/include/tele_manifest.h`
- Create: `components/tele_manifest/tele_manifest.c`
- Create: `components/tele_manifest/tele_manifest_http.c`
- Create: `components/tele_manifest/tele_manifest_json.c`
- Create: `components/tele_manifest/tele_manifest_sha256.c`
- Modify: root `CMakeLists.txt` only if this project requires explicit component registration.

- [x] **Step 1: Add component metadata**

Create `components/tele_manifest/idf_component.yml`:

```yaml
version: 0.1.0
description: Generic HTTPS manifest updater with file and streaming artifact apply.
license: MIT
repository: https://github.com/maujabur/TeleSystem.git
url: https://github.com/maujabur/TeleSystem/tree/main/components/tele_manifest
dependencies:
  idf: ">=5.3"
  espressif/cjson: "^1.7.19"
```

- [x] **Step 2: Add component build file**

Create `components/tele_manifest/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS
        "tele_manifest.c"
        "tele_manifest_http.c"
        "tele_manifest_json.c"
        "tele_manifest_sha256.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        cjson
        esp_http_client
        esp-tls
        mbedtls
)
```

- [x] **Step 3: Add Kconfig**

Create `components/tele_manifest/Kconfig`:

```text
menu "Tele manifest updater"

    config TELE_MANIFEST_HTTP_TIMEOUT_MS
        int "HTTP timeout in milliseconds"
        default 15000

    config TELE_MANIFEST_MAX_REDIRECTS
        int "Maximum HTTP redirects"
        default 5

    config TELE_MANIFEST_DOWNLOAD_BUFFER_SIZE
        int "HTTP artifact download buffer size"
        default 4096

    config TELE_MANIFEST_DEFAULT_MAX_MANIFEST_SIZE
        int "Default maximum manifest size"
        default 4096

endmenu
```

- [x] **Step 4: Add public header with final API surface**

Create `components/tele_manifest/include/tele_manifest.h` with:

```c
#ifndef TELE_MANIFEST_H
#define TELE_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TELE_MANIFEST_ARTIFACT_TYPE_SIZE 32
#define TELE_MANIFEST_CHANNEL_SIZE 32
#define TELE_MANIFEST_VERSION_SIZE 32
#define TELE_MANIFEST_BUILD_ID_SIZE 64
#define TELE_MANIFEST_URL_SIZE 384
#define TELE_MANIFEST_MAX_URLS 4
#define TELE_MANIFEST_SHA256_HEX_SIZE 65
#define TELE_MANIFEST_ERROR_SIZE 128

typedef struct {
    int schema;
    char artifact_type[TELE_MANIFEST_ARTIFACT_TYPE_SIZE];
    char channel[TELE_MANIFEST_CHANNEL_SIZE];
    char version[TELE_MANIFEST_VERSION_SIZE];
    char build_id[TELE_MANIFEST_BUILD_ID_SIZE];
    char urls[TELE_MANIFEST_MAX_URLS][TELE_MANIFEST_URL_SIZE];
    size_t url_count;
    char sha256_hex[TELE_MANIFEST_SHA256_HEX_SIZE];
    size_t size;
    char min_version[TELE_MANIFEST_VERSION_SIZE];
    bool critical;
} tele_manifest_artifact_t;

typedef enum {
    TELE_MANIFEST_RESULT_UNKNOWN = 0,
    TELE_MANIFEST_RESULT_SKIPPED_CURRENT,
    TELE_MANIFEST_RESULT_APPLIED,
    TELE_MANIFEST_RESULT_REJECTED_BY_POLICY,
    TELE_MANIFEST_RESULT_FAILED,
} tele_manifest_result_t;

typedef enum {
    TELE_MANIFEST_VERSION_REJECT = 0,
    TELE_MANIFEST_VERSION_SKIP_CURRENT,
    TELE_MANIFEST_VERSION_APPLY,
} tele_manifest_version_decision_t;

typedef struct {
    tele_manifest_result_t result;
    esp_err_t err;
    int http_status;
    size_t bytes_received;
    char selected_url[TELE_MANIFEST_URL_SIZE];
    char message[TELE_MANIFEST_ERROR_SIZE];
} tele_manifest_run_result_t;

typedef tele_manifest_version_decision_t (*tele_manifest_version_cb_t)(
    const tele_manifest_artifact_t *artifact,
    void *ctx);

typedef esp_err_t (*tele_manifest_file_apply_cb_t)(
    const char *verified_path,
    const tele_manifest_artifact_t *artifact,
    void *ctx);

typedef esp_err_t (*tele_manifest_stream_begin_cb_t)(
    const tele_manifest_artifact_t *artifact,
    void *ctx);

typedef esp_err_t (*tele_manifest_stream_write_cb_t)(
    const uint8_t *data,
    size_t data_len,
    void *ctx);

typedef esp_err_t (*tele_manifest_stream_finish_cb_t)(
    const tele_manifest_artifact_t *artifact,
    void *ctx);

typedef void (*tele_manifest_stream_abort_cb_t)(void *ctx);

typedef void (*tele_manifest_progress_cb_t)(
    const tele_manifest_artifact_t *artifact,
    size_t received,
    size_t total,
    void *ctx);

typedef struct {
    const char *artifact_type;
    const char *channel;
    size_t max_manifest_size;
    size_t max_artifact_size;
    tele_manifest_version_cb_t version_policy;
    tele_manifest_progress_cb_t progress;
    void *ctx;
} tele_manifest_request_t;

typedef struct {
    tele_manifest_stream_begin_cb_t begin;
    tele_manifest_stream_write_cb_t write;
    tele_manifest_stream_finish_cb_t finish;
    tele_manifest_stream_abort_cb_t abort;
} tele_manifest_stream_apply_t;

esp_err_t tele_manifest_fetch(const char *manifest_url,
                              const tele_manifest_request_t *request,
                              tele_manifest_artifact_t *out_artifact);

esp_err_t tele_manifest_apply_stream(const tele_manifest_artifact_t *artifact,
                                     const tele_manifest_request_t *request,
                                     const tele_manifest_stream_apply_t *apply,
                                     tele_manifest_run_result_t *out_result);

esp_err_t tele_manifest_run_stream(const char *manifest_url,
                                   const tele_manifest_request_t *request,
                                   const tele_manifest_stream_apply_t *apply,
                                   tele_manifest_run_result_t *out_result);

esp_err_t tele_manifest_apply_file(const tele_manifest_artifact_t *artifact,
                                   const tele_manifest_request_t *request,
                                   const char *work_dir,
                                   tele_manifest_file_apply_cb_t apply,
                                   tele_manifest_run_result_t *out_result);

esp_err_t tele_manifest_run_file(const char *manifest_url,
                                 const tele_manifest_request_t *request,
                                 const char *work_dir,
                                 tele_manifest_file_apply_cb_t apply,
                                 tele_manifest_run_result_t *out_result);

#ifdef __cplusplus
}
#endif

#endif
```

- [x] **Step 5: Add compiling stubs**

Add empty implementations that return `ESP_ERR_NOT_SUPPORTED` for public functions, then run:

```bash
idf.py build
```

Expected: build succeeds or fails only because current workspace configuration is already broken. If the failure is from missing symbols in `tele_manifest`, fix the skeleton before continuing.

- [x] **Step 6: Commit**

```bash
git add components/tele_manifest
git commit -m "feat: add manifest updater component skeleton"
```

## Task 2: Implement Manifest JSON Parsing

**Files:**

- Modify: `components/tele_manifest/tele_manifest_json.c`
- Modify: `components/tele_manifest/tele_manifest.c`
- Test: host-style parsing test if the project host test pattern supports `cJSON`; otherwise add an ESP-IDF component test under `components/tele_manifest/test/`.

- [x] **Step 1: Add parser helpers**

Implement:

```c
esp_err_t tele_manifest_parse_json(const char *text,
                                   const tele_manifest_request_t *request,
                                   tele_manifest_artifact_t *out_artifact);
```

Rules:

- Reject null pointers.
- Reject non-object JSON.
- Require `schema == 1`.
- Require matching `artifact_type`.
- Require non-empty `version`.
- Require `url` or `urls`.
- Accept both `url` and `urls`, normalize into `artifact.urls`.
- Reject more than `TELE_MANIFEST_MAX_URLS`.
- Reject non-HTTPS URLs.
- Require 64-character hex `sha256`.
- Require positive `size`.
- Enforce `request->max_artifact_size` when non-zero.
- Enforce configured channel when `request->channel` is non-empty.

- [x] **Step 2: Wire `tele_manifest_fetch` to parser**

For now, `tele_manifest_fetch()` may call an internal HTTP text fetch stub until Task 3. It must allocate at most `request->max_manifest_size` or `CONFIG_TELE_MANIFEST_DEFAULT_MAX_MANIFEST_SIZE`.

- [x] **Step 3: Add tests**

Cover at least:

- valid firmware manifest with `url`;
- valid CA manifest with `urls`;
- wrong `artifact_type`;
- wrong `channel`;
- bad SHA-256 length;
- non-HTTPS URL;
- oversized artifact;
- too many URLs.

- [x] **Step 4: Run tests**

Run the smallest available command for component tests. If no host test harness exists, run:

```bash
idf.py build
```

Expected: parser code compiles and all available parser tests pass.

- [x] **Step 5: Commit**

```bash
git add components/tele_manifest
git commit -m "feat: parse and validate update manifests"
```

## Task 3: Implement HTTPS Fetch And Artifact Download

**Files:**

- Modify: `components/tele_manifest/tele_manifest_http.c`
- Modify: `components/tele_manifest/tele_manifest_sha256.c`
- Modify: `components/tele_manifest/tele_manifest.c`
- Modify: `components/tele_manifest/CMakeLists.txt`

- [x] **Step 1: Implement HTTPS manifest fetch**

Add internal function:

```c
esp_err_t tele_manifest_http_get_text(const char *url,
                                      char *out_text,
                                      size_t out_size,
                                      int *out_status);
```

Use `esp_http_client_config_t` with:

- `.url = url`
- `.timeout_ms = CONFIG_TELE_MANIFEST_HTTP_TIMEOUT_MS`
- `.crt_bundle_attach = esp_crt_bundle_attach`
- `.keep_alive_enable = true`
- `.max_redirection_count = CONFIG_TELE_MANIFEST_MAX_REDIRECTS`
- `.buffer_size = CONFIG_TELE_MANIFEST_DOWNLOAD_BUFFER_SIZE`
- `.buffer_size_tx = CONFIG_TELE_MANIFEST_DOWNLOAD_BUFFER_SIZE`

- [x] **Step 2: Implement SHA-256 helpers**

Add helpers that hide mbedTLS details:

```c
esp_err_t tele_manifest_sha256_hex_to_bytes(const char *hex, uint8_t out[32]);
esp_err_t tele_manifest_sha256_bytes_to_hex(const uint8_t bytes[32], char out[65]);
```

Prefer public mbedTLS SHA-256 APIs available in the project ESP-IDF. If the current ESP-IDF only exposes the same private header used by the reference repo, isolate that include in `tele_manifest_sha256.c`.

- [x] **Step 3: Implement streaming artifact apply**

Implement `tele_manifest_apply_stream()`:

- Try artifact URLs in order.
- Open HTTPS connection.
- Check HTTP status is 200.
- If content length is present, require it to match `artifact->size`.
- Call `apply->begin(artifact, ctx)` exactly once after headers are accepted.
- Read chunks into a buffer of `CONFIG_TELE_MANIFEST_DOWNLOAD_BUFFER_SIZE`.
- Before each write, ensure `received + chunk_len <= artifact->size`.
- Update SHA-256 before or after successful write, but use the same bytes written.
- Call progress callback after successful write.
- When stream ends, require total bytes equal `artifact->size`.
- Compare SHA-256 against manifest.
- Call `apply->finish(artifact, ctx)` only after size and hash match.
- On any error after begin, call `apply->abort(ctx)`.

- [x] **Step 4: Implement file apply as wrapper**

Implement `tele_manifest_apply_file()` using the same artifact download mechanics but writing to a temp file under `work_dir`. Keep this path for CA bundle updates and future non-OTA artifacts.

- [x] **Step 5: Implement run helpers**

`tele_manifest_run_stream()` and `tele_manifest_run_file()` should:

- fetch manifest;
- apply version policy;
- skip current version cleanly;
- call the selected apply mode;
- populate `tele_manifest_run_result_t`.

- [x] **Step 6: Build**

```bash
idf.py build
```

Expected: build passes.

- [x] **Step 7: Commit**

```bash
git add components/tele_manifest
git commit -m "feat: add HTTPS manifest artifact delivery"
```

## Task 4: Add CA Store Component

**Files:**

- Create: `components/tele_ca_store/CMakeLists.txt`
- Create: `components/tele_ca_store/Kconfig`
- Create: `components/tele_ca_store/idf_component.yml`
- Create: `components/tele_ca_store/include/tele_ca_store.h`
- Create: `components/tele_ca_store/tele_ca_store.c`
- Modify: `partitions.csv` if the project needs a `ca_store` SPIFFS partition.

- [x] **Step 1: Decide storage partition**

The current `partitions.csv` has no SPIFFS. Add a CA storage partition only if runtime CA updates are required in this firmware image:

```csv
ca_store, data, spiffs, , 0x80000,
```

If flash space is tight, reduce app slot size only after checking current firmware binary size and target device flash size.

- [x] **Step 2: Add public CA API**

Create `tele_ca_store.h` with:

```c
#ifndef TELE_CA_STORE_H
#define TELE_CA_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
```

- [x] **Step 3: Implement from reference design**

Use `ca_manager` from `mozilla_ca_spiffs_updater` as the implementation reference, preserving these properties:

- mount SPIFFS;
- load existing bundle on boot;
- fallback to embedded ESP-IDF bundle if stored bundle is absent/invalid;
- validate candidate bundle with `esp_crt_bundle_set()`;
- promote only after validation succeeds;
- keep previous active bundle on failure;
- persist active version separately.

- [x] **Step 4: Wire app initialization**

Call `tele_ca_store_init()` during boot before HTTPS clients that should use the stored CA bundle.

- [x] **Step 5: Build**

```bash
idf.py build
```

Expected: build passes.

- [x] **Step 6: Commit**

```bash
git add components/tele_ca_store partitions.csv main components
git commit -m "feat: add runtime CA bundle store"
```

## Task 5: Add CA Manifest Adapter

**Files:**

- Create: `components/tele_ca_updater/CMakeLists.txt`
- Create: `components/tele_ca_updater/Kconfig`
- Create: `components/tele_ca_updater/idf_component.yml`
- Create: `components/tele_ca_updater/include/tele_ca_updater.h`
- Create: `components/tele_ca_updater/tele_ca_updater.c`
- Modify: `main/main.c`

- [ ] **Step 1: Add public adapter API**

Create:

```c
#ifndef TELE_CA_UPDATER_H
#define TELE_CA_UPDATER_H

#include "esp_err.h"
#include "tele_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *manifest_url;
    const char *channel;
    bool restart_on_update;
} tele_ca_updater_config_t;

esp_err_t tele_ca_updater_check(const tele_ca_updater_config_t *config,
                                tele_manifest_artifact_t *out_artifact);

esp_err_t tele_ca_updater_apply(const tele_ca_updater_config_t *config,
                                tele_manifest_run_result_t *out_result);

#ifdef __cplusplus
}
#endif

#endif
```

- [ ] **Step 2: Implement version policy**

CA version policy:

- Skip if manifest version equals stored CA version.
- Apply if stored version is empty.
- Apply if manifest version differs.
- Reject only when manifest channel/type validation fails in `tele_manifest`.

- [ ] **Step 3: Implement file apply callback**

The apply callback must call:

```c
tele_ca_store_apply_file(verified_path, artifact->version);
```

After success, restart only if `config->restart_on_update` is true.

- [ ] **Step 4: Wire into app**

Add app configuration for CA manifest URL and channel. In the first integration, run one manual or boot-time CA update check only after Wi-Fi is connected.

- [ ] **Step 5: Build**

```bash
idf.py build
```

Expected: build passes.

- [ ] **Step 6: Commit**

```bash
git add components/tele_ca_updater main
git commit -m "feat: add manifest-based CA updater"
```

## Task 6: Evolve Firmware OTA For Manifest Streaming

**Files:**

- Modify: `components/tele_system/include/firmware_ota.h`
- Modify: `components/tele_system/firmware_ota.c`
- Modify: `components/tele_system/CMakeLists.txt`
- Modify: `components/tele_system/idf_component.yml`
- Modify: `components/tele_system/Kconfig`
- Modify: `components/tele_system/include/firmware_version.h`

- [ ] **Step 1: Add normalized version macros**

Update `firmware_version.h` so firmware automation can compare a stable value:

```c
#define APP_VERSION_SEMVER "0.6.10"
#define APP_VERSION_LABEL "remote manifest ota"
#define APP_BUILD_ID "2026-06-24T12:00:00Z-local"
#define APP_VERSION_STRING APP_VERSION_SEMVER " " APP_VERSION_LABEL
```

Use the actual project version for `APP_VERSION_SEMVER`.

- [ ] **Step 2: Expand OTA status**

Add fields:

```c
char target_version[64];
char build_id[64];
char manifest_url[384];
char artifact_url[384];
size_t bytes_written;
size_t total_size;
uint8_t progress_pct;
```

- [ ] **Step 3: Add manifest OTA API**

Add:

```c
typedef struct {
    const char *manifest_url;
    const char *channel;
    bool allow_same_version;
    bool restart_on_success;
} firmware_ota_manifest_config_t;

esp_err_t firmware_ota_check_manifest(const firmware_ota_manifest_config_t *config,
                                      tele_manifest_artifact_t *out_artifact);

esp_err_t firmware_ota_start_manifest(const firmware_ota_manifest_config_t *config);
```

- [ ] **Step 4: Implement firmware version policy**

Policy:

- Reject `artifact_type` other than `firmware` through `tele_manifest`.
- Skip same version unless `allow_same_version` is true.
- Apply only if manifest semver is greater than `APP_VERSION_SEMVER`, or if `allow_same_version` is true.
- Reject if `min_version` exists and current firmware is lower than `min_version`.
- Reject if artifact size is larger than next OTA partition size.

- [ ] **Step 5: Implement stream callbacks**

Map manifest streaming callbacks to OTA:

- `begin`: call `esp_ota_get_next_update_partition()`, validate size, call `esp_ota_begin()`, set status running.
- `write`: call `esp_ota_write()`, update status bytes/progress.
- `finish`: call `esp_ota_end()`, call `esp_ota_set_boot_partition()`, set success/restart pending.
- `abort`: call `esp_ota_abort()` if active, clear active handle, set failed.

Do not set boot partition before `tele_manifest_apply_stream()` has validated size and SHA-256.

- [ ] **Step 6: Run manifest OTA in a task**

Like the existing URL OTA path, `firmware_ota_start_manifest()` should create a FreeRTOS task so callers do not block MQTT or HTTP handlers.

- [ ] **Step 7: Build**

```bash
idf.py build
```

Expected: build passes.

- [ ] **Step 8: Commit**

```bash
git add components/tele_system
git commit -m "feat: add manifest-driven streaming OTA"
```

## Task 7: Add Remote Commands For Check And Apply

**Files:**

- Modify: `main/main.c`
- Modify: `components/tele_commands/tele_commands.c` if commands are registered centrally.
- Modify: `docs/tele_commands.md`
- Modify: `docs/manual_mqtt_operacao.md`

- [ ] **Step 1: Add `ota_check` command**

Payload:

```json
{
  "cmd": "ota_check",
  "args": {
    "manifest_url": "https://updates.example.com/telesystem/pilot/manifest.json",
    "channel": "pilot"
  }
}
```

Response fields:

```json
{
  "ok": true,
  "current_version": "0.6.9",
  "available": true,
  "target_version": "0.6.10",
  "build_id": "2026-06-24T12:00:00Z-0.6.10",
  "size": 1286144,
  "critical": false
}
```

- [ ] **Step 2: Add `ota_apply` command**

Payload:

```json
{
  "cmd": "ota_apply",
  "args": {
    "manifest_url": "https://updates.example.com/telesystem/pilot/manifest.json",
    "channel": "pilot"
  }
}
```

Response should acknowledge that OTA started, not wait for reboot.

- [ ] **Step 3: Add `ca_check` and `ca_apply` commands**

Use the same shape as OTA, with artifact type fixed by the adapter.

- [ ] **Step 4: Update command manifest**

Expose the four commands in the command manifest so the desktop/MQTT control tools can discover them.

- [ ] **Step 5: Build**

```bash
idf.py build
```

Expected: build passes.

- [ ] **Step 6: Commit**

```bash
git add main components/tele_commands docs/tele_commands.md docs/manual_mqtt_operacao.md
git commit -m "feat: expose manifest update commands"
```

## Task 8: Surface Update Status In Portal And MQTT Status

**Files:**

- Modify: `main/main.c`
- Modify: `components/tele_portal_ota/tele_portal_ota.c`
- Modify: `components/tele_status/tele_status.c` if OTA status belongs in the status registry.
- Modify: `docs/tele_status.md`

- [ ] **Step 1: Extend `/api/ota/status`**

Return:

```json
{
  "state": "running",
  "current_version": "0.6.9",
  "target_version": "0.6.10",
  "build_id": "2026-06-24T12:00:00Z-0.6.10",
  "bytes_written": 524288,
  "total_size": 1286144,
  "progress_pct": 40,
  "manifest_url": "https://updates.example.com/telesystem/pilot/manifest.json",
  "artifact_url": "https://updates.example.com/telesystem/pilot/TeleSystem.bin",
  "last_error": ""
}
```

- [ ] **Step 2: Keep upload OTA working**

Manual upload status should still work. For upload mode, use:

- `manifest_url = ""`
- `artifact_url = "upload"`
- `target_version = ""` unless image metadata is parsed later.

- [ ] **Step 3: Add status registry fields if useful**

Expose update state, progress, and last error through `tele_status` only if those values are useful for MQTT dashboards. Do not duplicate the full artifact metadata unless consumers need it.

- [ ] **Step 4: Build**

```bash
idf.py build
```

Expected: build passes.

- [ ] **Step 5: Commit**

```bash
git add main components/tele_portal_ota components/tele_status docs/tele_status.md
git commit -m "feat: expose manifest update status"
```

## Task 9: Add Artifact Publishing Tooling

**Files:**

- Create: `tools/update_artifacts/generate_manifest.py`
- Create: `tools/update_artifacts/README.md`
- Modify: `.gitignore` if generated artifacts should stay out of the firmware repo.

- [ ] **Step 1: Add manifest generator**

The script should accept:

```bash
python tools/update_artifacts/generate_manifest.py \
  --artifact-type firmware \
  --channel pilot \
  --version 0.6.10 \
  --build-id 2026-06-24T12:00:00Z-0.6.10 \
  --url https://updates.example.com/telesystem/pilot/TeleSystem.bin \
  --file build/TeleSystem.bin \
  --out build/TeleSystem.manifest.json
```

Output JSON must include `schema`, `artifact_type`, `channel`, `version`, `build_id`, `url`, `sha256`, `size`, and `critical`.

- [ ] **Step 2: Support CA bundles**

The same script should work with:

```bash
python tools/update_artifacts/generate_manifest.py \
  --artifact-type ca_bundle \
  --channel stable \
  --version 2026.06.24 \
  --build-id 2026-06-24T12:00:00Z-ca \
  --url https://updates.example.com/ca/stable/bundle_ca.bin \
  --file artifacts/ca/stable/bundle_ca.bin \
  --out artifacts/ca/stable/bundle_ca.manifest.json
```

- [ ] **Step 3: Document publishing**

Document that artifacts should live in a separate artifact repository or release bucket, not in the firmware source tree.

- [ ] **Step 4: Run script locally**

Run the script against a small test file and inspect JSON manually.

- [ ] **Step 5: Commit**

```bash
git add tools/update_artifacts .gitignore
git commit -m "feat: add update manifest publishing tool"
```

## Task 10: Documentation And Integration Validation

**Files:**

- Modify: `docs/plano_ota_remoto_https.md`
- Modify: `docs/arquitetura_index.md`
- Create: `docs/manifest_updates.md`
- Modify: `README.md` if user-facing setup changed.

- [ ] **Step 1: Write architecture doc**

Create `docs/manifest_updates.md` with:

- component boundaries;
- manifest schema;
- CA update flow;
- firmware OTA update flow;
- command payload examples;
- failure modes;
- publishing workflow;
- known deferred work: signatures, rollout groups, ETag, auto scheduling.

- [ ] **Step 2: Update existing OTA plan**

Mark the implemented pieces in `docs/plano_ota_remoto_https.md` and remove outdated language that says remote manifest OTA is only future work.

- [ ] **Step 3: Add integration checklist**

Document hardware checks:

- boot with no CA bundle stored;
- CA manifest check with no update;
- CA manifest apply with valid bundle;
- OTA manifest check with no update;
- OTA manifest apply with valid firmware;
- wrong SHA-256 rejected;
- wrong size rejected;
- wrong channel rejected;
- URL returning 404 reported clearly;
- power loss during CA promotion preserves previous bundle;
- interrupted OTA does not change boot partition.

- [ ] **Step 4: Build**

```bash
idf.py build
```

Expected: build passes.

- [ ] **Step 5: Commit**

```bash
git add docs README.md
git commit -m "docs: document manifest update architecture"
```

## Verification Strategy

Run after the full implementation:

```bash
idf.py build
```

Expected: build succeeds.

Run component tests if available:

```bash
idf.py -T components/tele_manifest/test build
```

Expected: manifest parser and validation tests pass.

Run on hardware:

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

Expected:

- device boots;
- Wi-Fi connects;
- CA store initializes;
- current firmware version is reported;
- OTA upload page still works;
- `ota_check` returns current/available state;
- `ota_apply` downloads in chunks and reboots into the new partition;
- failed hash/size does not change boot partition.

## Deferred Work

- Signed manifests with embedded public keys.
- Rollout groups.
- Periodic auto-check with jitter.
- Auto-apply policy.
- ETag or `If-None-Match`.
- Non-HTTPS artifact mirrors after signed manifests are mandatory.
- Parsing app image metadata before writing all bytes.
- Progress persistence across reboot.

## Self-Review

- The plan covers both required artifact types: CA bundle and firmware.
- Firmware OTA uses streaming and does not require temporary firmware storage.
- CA bundle keeps file apply because bundle size is smaller and semantic validation needs a stable candidate file.
- The generic manifest layer does not know OTA partitions or CA bundle semantics.
- The domain adapters do not duplicate manifest parsing, HTTPS, URL validation, size checks, or SHA-256 logic.
- No retrocompatibility requirement is preserved when it conflicts with clean component boundaries.
