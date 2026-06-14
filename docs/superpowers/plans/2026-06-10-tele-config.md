# Tele Config Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a reusable ESP-IDF configuration layer where Kconfig provides defaults, NVS stores only explicit overrides, and callers can read/write/reset effective values through typed APIs.

**Architecture:** Add a small `components/tele_config` core that owns field registration, validation, NVS override storage, and schema export. Existing components keep declaring their Kconfig defaults, then register fields with `tele_config`; MQTT and web adapters consume the same field registry later instead of duplicating validation. Defaults must never be automatically written to NVS: NVS means override only.

**Tech Stack:** ESP-IDF C component, NVS, cJSON, existing `tele_wifi`, `tele_presence`, `tele_portal`, and `idf.py build`.

---

## File Structure

- Create `components/tele_config/CMakeLists.txt`: registers the reusable config component.
- Create `components/tele_config/include/tele_config.h`: public API, field descriptors, value type, flags, validation contracts.
- Create `components/tele_config/tele_config.c`: registry, validation, NVS get/set/reset, effective value resolution, schema JSON helpers.
- Create `components/tele_config/test/tele_config_validation_test.c`: pure validation tests that do not need NVS.
- Modify `CMakeLists.txt`: no change expected at project root.
- Modify `components/tele_wifi/CMakeLists.txt`: add `tele_config` dependency when migrating Wi-Fi fields.
- Modify `components/tele_wifi/device_config_store.c`: replace duplicated Kconfig/NVS logic for selected fields with `tele_config`.
- Modify `components/tele_wifi/include/device_config_store.h`: preserve existing public API so callers do not change in Task 1.
- Modify `components/tele_presence/CMakeLists.txt`: add `tele_config` only when MQTT adapter task begins.
- Modify `components/tele_presence/mqtt_presence.c`: later, build/apply settings through `tele_config`.
- Modify `main/connectivity/device_config_routes.c`: later, expose generic config through web while preserving `/api/device/config`.
- Modify docs: `docs/roadmap_atual.md` and architecture docs after code lands.

## Scope Guardrails

- Do not move Wi-Fi credentials (`wifi_config`) into `tele_config` in this pass.
- Do not auto-persist Kconfig defaults to NVS.
- Do not generate Kconfig entries dynamically; Kconfig remains compile-time.
- Do not implement web UI generation in the core component.
- Do not require every field to be public: secret/internal flags must exist from the start.

## Field Model

Initial supported types:

```c
typedef enum {
    TELE_CONFIG_TYPE_BOOL = 0,
    TELE_CONFIG_TYPE_I32,
    TELE_CONFIG_TYPE_U32,
    TELE_CONFIG_TYPE_STRING,
    TELE_CONFIG_TYPE_ENUM,
} tele_config_type_t;

typedef enum {
    TELE_CONFIG_FLAG_WEB = 1U << 0,
    TELE_CONFIG_FLAG_MQTT = 1U << 1,
    TELE_CONFIG_FLAG_SECRET = 1U << 2,
    TELE_CONFIG_FLAG_REBOOT_REQUIRED = 1U << 3,
    TELE_CONFIG_FLAG_READ_ONLY = 1U << 4,
} tele_config_flags_t;
```

Initial fields to migrate:

| ID | Type | Default | Range | Flags |
|---|---|---|---|---|
| `wifi.provisioning_ssid` | string | `CONFIG_WIFI_PROVISIONING_SSID` | 1..32 chars | WEB, MQTT |
| `wifi.sta_max_retry` | u32 | `CONFIG_WIFI_STA_MAX_RETRY` | 1..20 | WEB, MQTT |
| `wifi.apsta_policy` | enum/u32 | `CONFIG_WIFI_APSTA_POLICY` | 0..2 | WEB, MQTT |
| `wifi.apsta_grace_period_s` | u32 | `CONFIG_WIFI_APSTA_GRACE_PERIOD_S` | 30..3600 | WEB, MQTT |

## Task 1: Core Types and Validation

**Files:**
- Create: `components/tele_config/CMakeLists.txt`
- Create: `components/tele_config/include/tele_config.h`
- Create: `components/tele_config/tele_config.c`
- Create: `components/tele_config/test/tele_config_validation_test.c`

- [ ] **Step 1: Write validation test**

Create `components/tele_config/test/tele_config_validation_test.c`:

```c
#include <assert.h>
#include <string.h>

#include "tele_config.h"

static void test_u32_range_validation(void)
{
    tele_config_field_t field = {
        .id = "wifi.sta_max_retry",
        .type = TELE_CONFIG_TYPE_U32,
        .default_value.u32 = 3,
        .min.u32 = 1,
        .max.u32 = 20,
    };
    tele_config_value_t value = {.u32 = 0};
    assert(tele_config_validate_value(&field, &value) == ESP_ERR_INVALID_ARG);
    value.u32 = 1;
    assert(tele_config_validate_value(&field, &value) == ESP_OK);
    value.u32 = 20;
    assert(tele_config_validate_value(&field, &value) == ESP_OK);
    value.u32 = 21;
    assert(tele_config_validate_value(&field, &value) == ESP_ERR_INVALID_ARG);
}

static void test_string_length_validation(void)
{
    tele_config_field_t field = {
        .id = "wifi.provisioning_ssid",
        .type = TELE_CONFIG_TYPE_STRING,
        .default_value.string = "TeleSystem",
        .min_len = 1,
        .max_len = 32,
    };
    tele_config_value_t value = {.string = ""};
    assert(tele_config_validate_value(&field, &value) == ESP_ERR_INVALID_ARG);
    value.string = "TeleSystem";
    assert(tele_config_validate_value(&field, &value) == ESP_OK);
    value.string = "123456789012345678901234567890123";
    assert(tele_config_validate_value(&field, &value) == ESP_ERR_INVALID_SIZE);
}

int main(void)
{
    test_u32_range_validation();
    test_string_length_validation();
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
gcc -Icomponents/tele_config/include \
    -I$IDF_PATH/components/esp_common/include \
    components/tele_config/test/tele_config_validation_test.c \
    components/tele_config/tele_config.c \
    -o /tmp/tele_config_validation_test
```

Expected: fails because `tele_config.h` and implementation do not exist.

- [ ] **Step 3: Implement public API and validation**

Create `components/tele_config/include/tele_config.h` with:

```c
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TELE_CONFIG_ID_MAX_LEN 48
#define TELE_CONFIG_STRING_MAX_LEN 128

typedef enum {
    TELE_CONFIG_TYPE_BOOL = 0,
    TELE_CONFIG_TYPE_I32,
    TELE_CONFIG_TYPE_U32,
    TELE_CONFIG_TYPE_STRING,
    TELE_CONFIG_TYPE_ENUM,
} tele_config_type_t;

typedef enum {
    TELE_CONFIG_FLAG_WEB = 1U << 0,
    TELE_CONFIG_FLAG_MQTT = 1U << 1,
    TELE_CONFIG_FLAG_SECRET = 1U << 2,
    TELE_CONFIG_FLAG_REBOOT_REQUIRED = 1U << 3,
    TELE_CONFIG_FLAG_READ_ONLY = 1U << 4,
} tele_config_flags_t;

typedef union {
    bool boolean;
    int32_t i32;
    uint32_t u32;
    const char *string;
} tele_config_default_value_t;

typedef union {
    int32_t i32;
    uint32_t u32;
} tele_config_limit_t;

typedef union {
    bool boolean;
    int32_t i32;
    uint32_t u32;
    const char *string;
} tele_config_value_t;

typedef struct {
    const char *id;
    tele_config_type_t type;
    tele_config_default_value_t default_value;
    tele_config_limit_t min;
    tele_config_limit_t max;
    size_t min_len;
    size_t max_len;
    uint32_t flags;
} tele_config_field_t;

esp_err_t tele_config_validate_value(const tele_config_field_t *field,
                                     const tele_config_value_t *value);

#ifdef __cplusplus
}
#endif
```

Create `components/tele_config/tele_config.c` with validation only:

```c
#include "tele_config.h"

#include <string.h>

esp_err_t tele_config_validate_value(const tele_config_field_t *field,
                                     const tele_config_value_t *value)
{
    if (!field || !value || !field->id || field->id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    switch (field->type) {
    case TELE_CONFIG_TYPE_BOOL:
        return ESP_OK;
    case TELE_CONFIG_TYPE_I32:
    case TELE_CONFIG_TYPE_ENUM:
        if (value->i32 < field->min.i32 || value->i32 > field->max.i32) {
            return ESP_ERR_INVALID_ARG;
        }
        return ESP_OK;
    case TELE_CONFIG_TYPE_U32:
        if (value->u32 < field->min.u32 || value->u32 > field->max.u32) {
            return ESP_ERR_INVALID_ARG;
        }
        return ESP_OK;
    case TELE_CONFIG_TYPE_STRING:
        if (!value->string) {
            return ESP_ERR_INVALID_ARG;
        }
        size_t len = strlen(value->string);
        if (len < field->min_len) {
            return ESP_ERR_INVALID_ARG;
        }
        if (field->max_len > 0 && len > field->max_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}
```

Create `components/tele_config/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "tele_config.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash espressif__cjson
)
```

- [ ] **Step 4: Run test to verify it passes**

Run:

```bash
gcc -Icomponents/tele_config/include \
    -I$IDF_PATH/components/esp_common/include \
    components/tele_config/test/tele_config_validation_test.c \
    components/tele_config/tele_config.c \
    -o /tmp/tele_config_validation_test && /tmp/tele_config_validation_test
```

Expected: exit code 0.

## Task 2: Registry and NVS Overrides

**Files:**
- Modify: `components/tele_config/include/tele_config.h`
- Modify: `components/tele_config/tele_config.c`
- Create: `components/tele_config/test/tele_config_registry_test.c`

- [ ] **Step 1: Add tests for "default is not persisted"**

Create `components/tele_config/test/tele_config_registry_test.c` with a fake NVS section disabled for host compile. The host test must cover the registry behavior only:

```c
#include <assert.h>
#include <string.h>

#include "tele_config.h"

static const tele_config_field_t fields[] = {
    {
        .id = "wifi.sta_max_retry",
        .type = TELE_CONFIG_TYPE_U32,
        .default_value.u32 = 3,
        .min.u32 = 1,
        .max.u32 = 20,
        .flags = TELE_CONFIG_FLAG_WEB | TELE_CONFIG_FLAG_MQTT,
    },
};

int main(void)
{
    assert(tele_config_register_fields(fields, 1) == ESP_OK);
    assert(tele_config_find_field("wifi.sta_max_retry") == &fields[0]);
    assert(tele_config_find_field("missing") == NULL);
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
gcc -Icomponents/tele_config/include \
    -I$IDF_PATH/components/esp_common/include \
    components/tele_config/test/tele_config_registry_test.c \
    components/tele_config/tele_config.c \
    -o /tmp/tele_config_registry_test
```

Expected: fails because registry APIs do not exist.

- [ ] **Step 3: Add registry APIs**

Add to `tele_config.h`:

```c
esp_err_t tele_config_register_fields(const tele_config_field_t *fields, size_t field_count);
const tele_config_field_t *tele_config_find_field(const char *id);
```

Implement in `tele_config.c`:

```c
static const tele_config_field_t *s_fields;
static size_t s_field_count;

esp_err_t tele_config_register_fields(const tele_config_field_t *fields, size_t field_count)
{
    if (!fields || field_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    s_fields = fields;
    s_field_count = field_count;
    return ESP_OK;
}

const tele_config_field_t *tele_config_find_field(const char *id)
{
    if (!id || !s_fields) {
        return NULL;
    }
    for (size_t i = 0; i < s_field_count; ++i) {
        if (s_fields[i].id && strcmp(s_fields[i].id, id) == 0) {
            return &s_fields[i];
        }
    }
    return NULL;
}
```

- [ ] **Step 4: Add NVS override APIs**

Add to `tele_config.h`:

```c
esp_err_t tele_config_get_effective(const char *id,
                                    tele_config_value_t *out_value,
                                    char *string_buffer,
                                    size_t string_buffer_size,
                                    bool *out_from_nvs);
esp_err_t tele_config_set_override(const char *id, const tele_config_value_t *value);
esp_err_t tele_config_reset_override(const char *id);
```

Implement using namespace `"tele_config"` and key derived from field id by replacing `.` with `_`. Use these NVS types:

- bool -> `nvs_set_u8`;
- i32/enum -> `nvs_set_i32`;
- u32 -> `nvs_set_u32`;
- string -> `nvs_set_str`.

When `tele_config_get_effective()` does not find NVS key, return the Kconfig default from the descriptor and set `out_from_nvs=false`. Do not write the default to NVS.

- [ ] **Step 5: Run firmware build**

Run:

```bash
env XDG_CACHE_HOME=/tmp/telesystem-cache CCACHE_DIR=/tmp/telesystem-ccache idf.py build
```

Expected: build passes.

## Task 3: Migrate Wi-Fi Device Config Store Internals

**Files:**
- Modify: `components/tele_wifi/CMakeLists.txt`
- Modify: `components/tele_wifi/device_config_store.c`
- Modify: `components/tele_wifi/include/device_config_store.h`

- [ ] **Step 1: Register Wi-Fi fields**

In `device_config_store.c`, create a static field table:

```c
static const tele_config_field_t s_device_config_fields[] = {
    {
        .id = "wifi.provisioning_ssid",
        .type = TELE_CONFIG_TYPE_STRING,
        .default_value.string = CONFIG_WIFI_PROVISIONING_SSID,
        .min_len = 1,
        .max_len = 32,
        .flags = TELE_CONFIG_FLAG_WEB | TELE_CONFIG_FLAG_MQTT,
    },
    {
        .id = "wifi.sta_max_retry",
        .type = TELE_CONFIG_TYPE_U32,
        .default_value.u32 = CONFIG_WIFI_STA_MAX_RETRY,
        .min.u32 = DEVICE_CONFIG_STA_MAX_RETRY_MIN,
        .max.u32 = DEVICE_CONFIG_STA_MAX_RETRY_MAX,
        .flags = TELE_CONFIG_FLAG_WEB | TELE_CONFIG_FLAG_MQTT,
    },
    {
        .id = "wifi.apsta_policy",
        .type = TELE_CONFIG_TYPE_ENUM,
        .default_value.i32 = CONFIG_WIFI_APSTA_POLICY,
        .min.i32 = DEVICE_CONFIG_APSTA_ALWAYS_ON,
        .max.i32 = DEVICE_CONFIG_APSTA_STA_ONLY,
        .flags = TELE_CONFIG_FLAG_WEB | TELE_CONFIG_FLAG_MQTT,
    },
    {
        .id = "wifi.apsta_grace_period_s",
        .type = TELE_CONFIG_TYPE_U32,
        .default_value.u32 = CONFIG_WIFI_APSTA_GRACE_PERIOD_S,
        .min.u32 = DEVICE_CONFIG_APSTA_GRACE_PERIOD_S_MIN,
        .max.u32 = DEVICE_CONFIG_APSTA_GRACE_PERIOD_S_MAX,
        .flags = TELE_CONFIG_FLAG_WEB | TELE_CONFIG_FLAG_MQTT,
    },
};
```

Expose:

```c
esp_err_t device_config_store_register_fields(void);
```

and call `tele_config_register_fields()` inside it.

- [ ] **Step 2: Preserve existing API**

Rewrite existing functions to call `tele_config_get_effective()` and `tele_config_set_override()`:

- `device_config_store_load_provisioning_ssid`
- `device_config_store_save_provisioning_ssid`
- `device_config_store_load_sta_max_retry`
- `device_config_store_save_sta_max_retry`
- `device_config_store_load_apsta_policy`
- `device_config_store_save_apsta_policy`

Do not save defaults while loading.

- [ ] **Step 3: Register fields during boot**

In `main/main.c`, after `nvs_flash_init()` succeeds and before route/MQTT registration, call:

```c
ESP_ERROR_CHECK(device_config_store_register_fields());
```

- [ ] **Step 4: Build and validate behavior**

Run:

```bash
env XDG_CACHE_HOME=/tmp/telesystem-cache CCACHE_DIR=/tmp/telesystem-ccache idf.py build
```

Expected: build passes.

Manual hardware validation:

1. Erase NVS or use fresh device.
2. Boot.
3. `/api/device/config` returns Kconfig defaults.
4. Change `sta_max_retry` via portal.
5. Reboot.
6. `/api/device/config` returns NVS override.
7. Reset support is not required yet in this task.

## Task 4: MQTT Adapter Uses tele_config

**Files:**
- Modify: `components/tele_presence/CMakeLists.txt`
- Modify: `components/tele_presence/mqtt_presence.c`

- [ ] **Step 1: Replace manual settings JSON for migrated fields**

In `mqtt_presence_build_settings()`, build `device_connectivity` by reading effective values from `tele_config` instead of direct `device_config_store_*` calls.

- [ ] **Step 2: Replace manual validation for migrated fields**

In `mqtt_presence_apply_settings()`, call `tele_config_set_override()` for each migrated field, then call runtime apply functions:

- `wifi_manager_set_provisioning_ssid`
- `wifi_manager_set_sta_max_retry`
- `wifi_manager_set_apsta_policy`

Return existing error strings for compatibility.

- [ ] **Step 3: Build**

Run:

```bash
env XDG_CACHE_HOME=/tmp/telesystem-cache CCACHE_DIR=/tmp/telesystem-ccache idf.py build
```

Expected: build passes.

## Task 5: Web Adapter Path

**Files:**
- Modify: `main/connectivity/device_config_routes.c`

- [ ] **Step 1: Preserve existing `/api/device/config`**

Keep the current endpoint shape so existing HTML keeps working.

- [ ] **Step 2: Use tele_config under the hood**

Replace direct `device_config_store_*` calls with `tele_config` reads/writes or keep wrapper calls if Task 3 wrappers already use `tele_config`.

- [ ] **Step 3: Add reset support only if needed**

Do not add a new UI yet. If reset is needed for testing, accept:

```json
{"reset":["wifi.sta_max_retry"]}
```

and call `tele_config_reset_override()`.

- [ ] **Step 4: Build**

Run:

```bash
env XDG_CACHE_HOME=/tmp/telesystem-cache CCACHE_DIR=/tmp/telesystem-ccache idf.py build
```

Expected: build passes.

## Task 6: Documentation

**Files:**
- Modify: `docs/roadmap_atual.md`
- Modify: `docs/main_raiz_estrutura_alto_nivel.md`
- Modify: `docs/main_connectivity_estrutura_alto_nivel.md`
- Add or modify: `docs/tele_config.md`

- [ ] **Step 1: Document the rule**

Create `docs/tele_config.md` with:

```markdown
# Tele Config

`tele_config` centraliza campos configuraveis reutilizaveis.

Regra principal:

- Kconfig define o default compilado.
- NVS guarda apenas override explicito.
- Valor efetivo = override NVS, se existir; senao default Kconfig.
- Reset apaga override NVS e volta a seguir o default do firmware.

Defaults nao sao gravados automaticamente na NVS.
```

- [ ] **Step 2: Update architecture index**

Add `tele_config` to the component list and link `docs/tele_config.md`.

- [ ] **Step 3: Final build verification**

Run:

```bash
env XDG_CACHE_HOME=/tmp/telesystem-cache CCACHE_DIR=/tmp/telesystem-ccache idf.py build
```

Expected: build passes.

