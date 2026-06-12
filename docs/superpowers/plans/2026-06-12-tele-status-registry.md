# Tele Status Registry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a reusable read-only `tele_status` registry and use it as the source for current MQTT state and heartbeat payloads.

**Architecture:** `components/tele_status` owns field metadata and value getters. `components/tele_presence` registers common runtime fields and asks `tele_status` to build JSON payloads, preserving the existing MQTT topic contract for this slice. Manifest publishing and dynamic desktop rendering stay out of scope.

**Tech Stack:** ESP-IDF C components, cJSON, host-compiled C tests with lightweight ESP error stubs.

---

### Task 1: Registry API And Host Test

**Files:**
- Create: `components/tele_status/include/tele_status.h`
- Create: `components/tele_status/tele_status.c`
- Create: `components/tele_status/test/tele_status_registry_test.c`
- Create: `components/tele_status/CMakeLists.txt`

- [ ] **Step 1: Write failing host test**

Create a test that registers bool, u32, i32, and string fields, reads them into JSON, rejects duplicate IDs, and verifies filtering by flags.

- [ ] **Step 2: Run test and verify failure**

Run:

```bash
gcc -DTELE_STATUS_HOST_TEST -Icomponents/tele_status/include components/tele_status/test/tele_status_registry_test.c components/tele_status/tele_status.c -o /tmp/tele_status_registry_test
```

Expected before implementation: compile failure because the component does not exist.

- [ ] **Step 3: Implement minimal registry**

Implement:

```c
esp_err_t tele_status_register_fields(const tele_status_field_t *fields, size_t field_count);
const tele_status_field_t *tele_status_find_field(const char *id);
esp_err_t tele_status_add_fields_to_json(cJSON *root, uint32_t required_flags);
```

- [ ] **Step 4: Run host test**

Run:

```bash
gcc -DTELE_STATUS_HOST_TEST -Icomponents/tele_status/include components/tele_status/test/tele_status_registry_test.c components/tele_status/tele_status.c -o /tmp/tele_status_registry_test && /tmp/tele_status_registry_test
```

Expected: exit code 0.

### Task 2: MQTT Presence Integration

**Files:**
- Modify: `components/tele_presence/mqtt_presence.c`
- Modify: `components/tele_presence/CMakeLists.txt`

- [ ] **Step 1: Register common runtime status fields**

Register fields for `wifi_state`, `wifi_ready`, `ssid`, `ip`, `rssi`, `vbat_mv`, `heap_free`, `uptime_s`, `heartbeat_interval_s`, and `time_synchronized`.

- [ ] **Step 2: Use registry for state and heartbeat JSON**

Make `mqtt_presence_build_state()` and `mqtt_presence_build_heartbeat()` call `tele_status_add_fields_to_json()` with flags that preserve the current payload shape.

- [ ] **Step 3: Preserve technical status**

Keep `get_technical_status` manually assembled for now, because its nested objects are richer than the first registry slice.

### Task 3: Documentation, Version, And Verification

**Files:**
- Create: `docs/tele_status.md`
- Modify: `components/tele_system/include/firmware_version.h`
- Modify: `docs/manual_mqtt_operacao.md`
- Modify: `docs/plano_ota_remoto_https.md`

- [ ] **Step 1: Document the component boundary**

Document that `tele_status` is read-only, has no NVS, and is the future source for `meta/status`.

- [ ] **Step 2: Bump firmware version**

Set the firmware string to the current feature version.

- [ ] **Step 3: Verify**

Run:

```bash
python3 -m py_compile tools/mqtt_desktop/mqtt_control_center.py
gcc -DTELE_STATUS_HOST_TEST -Icomponents/tele_status/include components/tele_status/test/tele_status_registry_test.c components/tele_status/tele_status.c -o /tmp/tele_status_registry_test && /tmp/tele_status_registry_test
CCACHE_DIR=/tmp/ccache idf.py build
```

Expected: all commands exit 0.
