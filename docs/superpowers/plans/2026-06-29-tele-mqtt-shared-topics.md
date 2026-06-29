# Tele MQTT Shared Topics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend `components/tele_mqtt` with a reusable shared-topic API so product/domain components can publish and subscribe to MQTT topics outside the per-device topic tree without coupling `tele_mqtt` to any product-specific behavior.

**Architecture:** `tele_mqtt` continues to own MQTT connection lifecycle, topic construction, publish calls, subscriptions, reconnect handling, and inbound message dispatch. Product/domain components register shared-topic handlers using topic suffixes. `tele_mqtt` composes full topics under a reserved shared namespace and dispatches matching messages to registered callbacks. Domain-specific JSON schemas, filtering, state machines, persistence, indicators, and business rules remain out of scope.

**Tech Stack:** ESP-IDF C components, `esp-mqtt`, cJSON only where already used by `tele_mqtt`, host-compiled C tests with lightweight ESP/MQTT stubs where practical, and full `idf.py build` verification.

---

## Non-Goals

- Do not implement any product-specific protocol, payload, state machine, sensor handling, GPIO handling, LED behavior, grouping logic, peer cache, timeout policy, or Control Center UI change.
- Do not change the existing per-device MQTT topic contract.
- Do not replace `tele_config`, `tele_status`, or `tele_commands`.
- Do not add retained shared payload semantics by default.
- Do not introduce topic wildcard dispatch unless explicitly needed by tests in this plan.

---

## Proposed Shared Topic Namespace

Existing per-device topics remain unchanged:

```text
{base_topic}/{device_id}/availability
{base_topic}/{device_id}/seen
{base_topic}/{device_id}/state
{base_topic}/{device_id}/heartbeat
{base_topic}/{device_id}/event
{base_topic}/{device_id}/meta/config
{base_topic}/{device_id}/meta/status
{base_topic}/{device_id}/meta/commands
{base_topic}/{device_id}/cmd/in
{base_topic}/{device_id}/cmd/out
```

Add a reserved shared namespace:

```text
{base_topic}/_shared/{topic_suffix}
```

Examples only for API shape, not product implementation:

```text
v1/telesystem/_shared/example/event
v1/telesystem/_shared/example/state
```

Rules:

- `topic_suffix` is relative and must not start with `/`.
- `topic_suffix` must not contain empty path segments.
- `topic_suffix` must not contain MQTT wildcards in the first implementation unless this plan is explicitly extended.
- Full topic construction is owned by `tele_mqtt`.
- Callers must not hardcode `base_topic` or `_shared`.

---

### Task 1: Public API And Topic Builder

**Files:**
- Modify: `components/tele_mqtt/include/tele_mqtt.h`
- Modify: `components/tele_mqtt/tele_mqtt.c`
- Create: `components/tele_mqtt/test/tele_mqtt_shared_topic_test.c`

- [ ] **Step 1: Add public callback type**

Add a callback type for shared-topic inbound messages:

```c
typedef esp_err_t (*tele_mqtt_shared_handler_t)(const char *topic,
                                                const char *payload,
                                                size_t payload_len,
                                                void *ctx);
```

The callback receives the full MQTT topic, raw payload bytes as a null-terminated buffer when possible, payload length, and caller context.

- [ ] **Step 2: Add public shared-topic APIs**

Add APIs equivalent to:

```c
esp_err_t tele_mqtt_subscribe_shared(const char *topic_suffix,
                                     int qos,
                                     tele_mqtt_shared_handler_t handler,
                                     void *ctx);

esp_err_t tele_mqtt_publish_shared(const char *topic_suffix,
                                   const char *payload,
                                   int qos,
                                   bool retain);
```

These APIs must be generic and must not mention any product or domain by name.

- [ ] **Step 3: Implement suffix validation**

Validate `topic_suffix` before storing or publishing:

- reject `NULL`;
- reject empty strings;
- reject strings starting with `/`;
- reject strings ending with `/`;
- reject empty segments such as `foo//bar`;
- reject `+` and `#` for the first implementation;
- reject suffixes that would overflow the internal topic buffer.

Return `ESP_ERR_INVALID_ARG` for invalid input.

- [ ] **Step 4: Implement shared full-topic builder**

Create an internal helper that builds:

```text
{base_topic}/_shared/{topic_suffix}
```

The helper must reuse the configured `base_topic` and must not require callers to know the device ID.

- [ ] **Step 5: Write host/unit test for topic validation and construction**

Create a test covering:

- valid suffix builds expected topic;
- invalid empty suffix fails;
- leading slash fails;
- trailing slash fails;
- double slash fails;
- wildcard suffix fails;
- overly long suffix fails.

Use lightweight stubs if `esp-mqtt` is not available in host tests.

---

### Task 2: Subscription Registry

**Files:**
- Modify: `components/tele_mqtt/tele_mqtt.c`
- Modify: `components/tele_mqtt/include/tele_mqtt.h` if needed
- Extend: `components/tele_mqtt/test/tele_mqtt_shared_topic_test.c`

- [ ] **Step 1: Add internal shared subscription registry**

Store registered shared subscriptions in an internal static table.

Each entry should contain:

```c
typedef struct {
    bool used;
    char suffix[...];
    char full_topic[...];
    int qos;
    tele_mqtt_shared_handler_t handler;
    void *ctx;
} tele_mqtt_shared_subscription_t;
```

Choose conservative fixed limits consistent with the existing `tele_mqtt` style and Kconfig/topic buffer sizes.

- [ ] **Step 2: Reject duplicate suffix registrations**

If the same suffix is registered twice, return `ESP_ERR_INVALID_STATE` or another existing project-consistent error.

Do not silently replace handlers.

- [ ] **Step 3: Enforce registry capacity**

If the registry is full, return `ESP_ERR_NO_MEM`.

- [ ] **Step 4: Subscribe immediately when MQTT client is already connected**

When `tele_mqtt_subscribe_shared()` is called after the client is connected, call `esp_mqtt_client_subscribe()` immediately with the full topic and requested QoS.

If the client is not connected yet, store the entry and subscribe during the next connect event.

- [ ] **Step 5: Add tests for registry behavior**

Test:

- first registration succeeds;
- duplicate registration fails;
- multiple distinct suffixes succeed;
- invalid handler fails;
- capacity overflow fails if the limit is reachable in the test.

---

### Task 3: Reconnect And Resubscribe

**Files:**
- Modify: `components/tele_mqtt/tele_mqtt.c`
- Extend: `components/tele_mqtt/test/tele_mqtt_shared_topic_test.c` if practical

- [ ] **Step 1: Resubscribe shared topics on MQTT connected event**

In the existing MQTT connected event handling, after the normal per-device subscriptions are restored, iterate the shared subscription registry and subscribe each active shared topic.

- [ ] **Step 2: Keep existing per-device subscriptions unchanged**

Verify that existing command topic subscription behavior is untouched.

No existing topic should be renamed or moved.

- [ ] **Step 3: Log shared subscriptions clearly**

Add concise logs for shared-topic registration and subscription attempts:

```text
shared subscribe suffix=... topic=... qos=...
```

Avoid logging payload contents.

- [ ] **Step 4: Handle subscribe failures without crashing**

If `esp_mqtt_client_subscribe()` fails for a shared topic, log the failure and keep the registry entry so reconnect can retry.

---

### Task 4: Publish API

**Files:**
- Modify: `components/tele_mqtt/tele_mqtt.c`
- Extend: `components/tele_mqtt/test/tele_mqtt_shared_topic_test.c`

- [ ] **Step 1: Implement `tele_mqtt_publish_shared()`**

Build the full shared topic from suffix and call the existing MQTT publish path or `esp_mqtt_client_publish()` consistently with the current implementation style.

- [ ] **Step 2: Validate publish arguments**

Reject invalid suffixes and invalid QoS values.

Allow empty payload if the existing MQTT publish conventions allow it. If not, document and enforce the existing convention.

- [ ] **Step 3: Return clear errors when client is unavailable**

If MQTT is not started or the client handle is unavailable, return a clear error consistent with existing `tele_mqtt` behavior.

- [ ] **Step 4: Keep retain explicit**

The publish API receives `retain` as an explicit argument. Do not retain shared messages by default inside `tele_mqtt`.

---

### Task 5: Inbound Dispatch

**Files:**
- Modify: `components/tele_mqtt/tele_mqtt.c`
- Extend: `components/tele_mqtt/test/tele_mqtt_shared_topic_test.c`

- [ ] **Step 1: Route inbound shared-topic messages before command parsing**

In the MQTT data event handler, detect whether the inbound topic matches any registered shared full topic.

If matched, call the registered handler and do not pass the message to per-device command parsing.

- [ ] **Step 2: Use exact-match dispatch in the first implementation**

Match full topic string exactly.

Do not implement wildcard matching in this slice.

- [ ] **Step 3: Preserve existing command behavior**

If the inbound topic does not match any shared subscription, preserve the existing path for `cmd/in` and other current handling.

- [ ] **Step 4: Handle payload safely**

MQTT payloads may not be null-terminated. Copy into a bounded temporary buffer or pass length-aware data consistently.

Callbacks receive `payload_len`. Do not require JSON.

- [ ] **Step 5: Test dispatch**

Test:

- matching shared topic invokes the registered handler;
- non-matching topic does not invoke handler;
- shared handler receives expected topic and payload length;
- command topic still follows existing logic or remains unclaimed by shared dispatch.

---

### Task 6: Kconfig Limits And Documentation

**Files:**
- Modify: `components/tele_mqtt/Kconfig` if present
- Modify: `docs/manual_mqtt_operacao.md`
- Modify: `docs/componentes_mqtt_config_status_commands.md`
- Modify: `README.md` if the public architecture summary needs one short note

- [ ] **Step 1: Add optional Kconfig limits if needed**

If fixed limits are not already available, add Kconfig entries such as:

```text
CONFIG_TELE_MQTT_MAX_SHARED_SUBSCRIPTIONS
CONFIG_TELE_MQTT_SHARED_TOPIC_SUFFIX_MAX_LEN
```

Use conservative defaults.

- [ ] **Step 2: Document the shared namespace**

Document:

```text
{base_topic}/_shared/{topic_suffix}
```

Explain that this namespace is for product/domain components that need shared MQTT topics outside the per-device tree.

- [ ] **Step 3: Document boundaries**

State explicitly:

- `tele_mqtt` owns connection, publish, subscribe, resubscribe, and dispatch;
- domain components own payload schema and state;
- shared topics do not replace `tele_config`, `tele_status`, or `tele_commands`;
- existing per-device topic contract remains unchanged.

- [ ] **Step 4: Add a generic example**

Use neutral names only:

```c
static esp_err_t example_shared_handler(const char *topic,
                                        const char *payload,
                                        size_t payload_len,
                                        void *ctx)
{
    (void)topic;
    (void)payload;
    (void)payload_len;
    (void)ctx;
    return ESP_OK;
}

ESP_ERROR_CHECK(tele_mqtt_subscribe_shared("example/event",
                                           1,
                                           example_shared_handler,
                                           NULL));

ESP_ERROR_CHECK(tele_mqtt_publish_shared("example/event",
                                         "{\"ok\":true}",
                                         1,
                                         false));
```

No product names in this documentation slice.

---

### Task 7: Version And Verification

**Files:**
- Modify: `main/app_firmware_version.h`
- Use existing build/test files as needed

- [ ] **Step 1: Bump firmware version**

Update `APP_FIRMWARE_VERSION_SEMVER` and `APP_FIRMWARE_VERSION_LABEL` to reflect the MQTT shared-topic feature.

Suggested label:

```c
#define APP_FIRMWARE_VERSION_LABEL "MQTT shared topics"
```

Use the next appropriate semver for the repository.

- [ ] **Step 2: Run host tests**

Run the new shared-topic test command. Adjust include paths to the actual component layout:

```bash
gcc -DTELE_MQTT_HOST_TEST \
    -Icomponents/tele_mqtt/include \
    components/tele_mqtt/test/tele_mqtt_shared_topic_test.c \
    components/tele_mqtt/tele_mqtt.c \
    -o /tmp/tele_mqtt_shared_topic_test && \
    /tmp/tele_mqtt_shared_topic_test
```

If `tele_mqtt.c` cannot be host-compiled cleanly because of ESP-IDF dependencies, split pure helpers into a small internal testable unit or use stubs consistent with existing project practice.

- [ ] **Step 3: Run full build**

Run:

```bash
idf.py build
```

Expected: build exits 0.

- [ ] **Step 4: Manual MQTT smoke test**

With a broker configured and MQTT enabled:

1. boot one device;
2. register a generic shared subscription from a small test/domain component;
3. publish to `{base_topic}/_shared/example/event`;
4. verify handler log fires once;
5. reboot or force reconnect;
6. publish again;
7. verify resubscribe works.

- [ ] **Step 5: Existing contract regression check**

Verify the device still publishes the existing retained and periodic topics:

```text
{base_topic}/{device_id}/availability
{base_topic}/{device_id}/seen
{base_topic}/{device_id}/state
{base_topic}/{device_id}/heartbeat
{base_topic}/{device_id}/meta/config
{base_topic}/{device_id}/meta/status
{base_topic}/{device_id}/meta/commands
```

Verify `cmd/in` and `cmd/out` still work with `ping` or `get_state`.

---

## Acceptance Criteria

- `tele_mqtt` exposes generic shared-topic subscribe and publish APIs.
- Shared topics are built only by `tele_mqtt` under `{base_topic}/_shared/{topic_suffix}`.
- Shared subscriptions survive MQTT reconnect by resubscribing on connected events.
- Inbound shared-topic messages are dispatched to registered callbacks by exact topic match.
- Existing per-device topics and command handling remain unchanged.
- The implementation contains no product-specific names, schemas, GPIO logic, sensor logic, grouping logic, LED behavior, or Control Center behavior.
- Documentation explains the new generic shared-topic capability and its boundaries.
- Host/unit tests pass where practical.
- `idf.py build` passes.
