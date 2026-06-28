# Tele Indicator Implementation Plan

> **For agentic workers:** Implement this plan task by task. Use the checkbox (`- [ ]`) items for progress tracking. Do not skip stages. Each stage must compile before moving to the next one. Prefer small, reviewable changes over broad rewrites. Keep the implementation suitable for ESP-IDF C on ESP32-S3.

**Goal:** Refactor the current visual indication system into a reusable `tele_indicator` architecture with shared visual signal types, output registration, event registration, priority arbitration, and event duration handling.

**Architecture:** `components/tele_signal` defines reusable visual signal types such as colors, effect IDs, timing fields, brightness, target masks, and flags. `components/status_led` controls the physical status LED and applies `tele_signal` effects without depending on `tele_indicator`. `components/tele_indicator` owns the registry of outputs, sources, and events, resolves the currently effective event by priority and recency, and applies the selected effect to the appropriate registered output. Application code registers product-specific indicator events in `app_indicators.c`.

**Tech Stack:** ESP-IDF C components, FreeRTOS mutexes, `esp_timer` for temporary events, fixed-size static registries, no unnecessary dynamic allocation, and host-compiled C tests with lightweight ESP error stubs where practical.

**Design Constraints:**

- `status_led` must not know about semantic application states such as Wi-Fi, battery, product status, MQTT, OTA, capture, or errors.
- `status_led` must depend only on `tele_signal`, not on `tele_indicator`.
- `tele_indicator` must own the policy layer: outputs, sources, events, priorities, active state, and durations.
- Event duration belongs to the event definition, not to the effect definition.
- The initial implementation must support one physical output, `status_led`, but the architecture must allow future outputs such as LED groups, buzzers, speakers, or displays.
- The global output registry must know which effects each output supports.
- Keep the implementation small, static, predictable, and suitable for ESP32-S3.
- `solid` and `off` may share internal implementation, but they must remain separate effect IDs for semantic clarity.
- `blink` and `alternate` may share internal implementation, but they must remain separate effect IDs for semantic clarity.
- Do not implement complex multi-LED effects such as `scanning`, `fill`, `chase`, `rainbow`, gradients, or scripting in this stage.
- Do not include the `reason` field in this implementation. It is not currently consumed and can be reintroduced later through an extended API if needed.
- Avoid abstraction beyond what is described in this plan.

---

## 1. Objective

Refactor the current visual indication system to clearly separate:

1. **Generic visual signal**
   Component: `tele_signal`
   Defines common types such as color, effect, timing, brightness, target mask, and flags.

2. **Physical status LED driver**
   Component: `status_led`
   Controls the physical LED and executes simple effects, but does not know about system events.

3. **Generic indicator manager**
   Component: `tele_indicator`
   Resolves policy, priority, duration, active events, and registered outputs.

4. **Product event table**
   Application file, for example: `app_indicators.c`
   Centralizes the relationship between semantic events and visual signals.

The intended architecture is:

```text
system events
    ↓
tele_indicator
    ↓
registered output
    ↓
status_led / buzzer / future indicators
```

---

## 2. Main Guidelines

- `status_led` should be a relatively dumb physical driver.
- `status_led` must not know Wi-Fi, battery, product, error, OTA, MQTT, capture, or any application-level semantic event.
- `tele_indicator` must resolve which indication is currently effective.
- `tele_indicator` must support multiple registered outputs.
- `tele_indicator` must know which outputs exist and which effects each output supports.
- `status_led` must not depend on `tele_indicator`.
- Create a shared `tele_signal` component to avoid cross-dependencies.
- Duration must belong to the event, not to the effect.
- Temporary events must be cleared automatically after their configured duration.
- `solid` and `off` may be internally close, but they must remain separate IDs for semantic clarity.
- `blink` and `alternate` may share internal implementation, but they must remain separate IDs for usage clarity.
- Do not implement complex effects such as `scanning`, `fill`, `rainbow`, `chase`, etc. now. Leave the structure ready for future support.

---

## 3. Desired Component Structure

```text
components/
  tele_signal/
    include/tele_signal.h
    CMakeLists.txt

  status_led/
    include/status_led.h
    status_led.c
    status_led_effects.c
    CMakeLists.txt

  tele_indicator/
    include/tele_indicator.h
    tele_indicator.c
    CMakeLists.txt

main/
  app_indicators.c
  app_indicators.h
```

---

# 4. Create the `tele_signal` Component

## File

```text
components/tele_signal/include/tele_signal.h
```

## Responsibility

Define generic signaling types reusable by `status_led`, `tele_indicator`, and future outputs.

## Proposed API

```c
#ifndef TELE_SIGNAL_H
#define TELE_SIGNAL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TELE_SIGNAL_ID_SIZE 32

#define TELE_SIGNAL_TARGET_ALL 0xFFFFFFFFu
#define TELE_SIGNAL_TARGET_0   (1u << 0)

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} tele_signal_color_t;

typedef struct {
    char id[TELE_SIGNAL_ID_SIZE];

    tele_signal_color_t color_a;
    tele_signal_color_t color_b;

    uint32_t time_a_ms;
    uint32_t time_b_ms;

    uint8_t brightness;

    uint32_t target_mask;
    uint32_t flags;
} tele_signal_effect_t;

bool tele_signal_effect_id_is_valid(const char *id);

#ifdef __cplusplus
}
#endif

#endif
```

## Initial Effect IDs

Use fixed strings:

```c
"off"
"solid"
"blink"
"alternate"
"breath"
"heartbeat"
"pulse"
```

## Notes

- `off` may be implemented as `solid` with color `{0, 0, 0}` and brightness `0`, but it must continue to exist as its own effect ID.
- `blink` may be implemented as `alternate` between `color_a` and off.
- `alternate` alternates between `color_a` and `color_b`.
- `breath` uses `color_a`, with `time_a_ms` as the main period.
- `heartbeat` uses `color_a` and either fixed internal timings or timings derived from `time_a_ms`.
- `pulse` may be a brief activation of `color_a` followed by off.
- Official event duration must still live in `tele_indicator`, not inside the effect.

---

# 5. Refactor `status_led`

## Responsibility

Control the physical LED and execute effects defined by `tele_signal_effect_t`.

`status_led` must no longer know semantic states such as:

```c
STATUS_LED_STATE_WIFI_CONNECTING
STATUS_LED_STATE_PRODUCT_TRANSMITTING
STATUS_LED_STATE_LOW_BATTERY
```

These states must be removed from `status_led`.

## Proposed New Header

```c
#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdbool.h>

#include "esp_err.h"
#include "tele_signal.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t status_led_start(void);

esp_err_t status_led_apply_effect(const tele_signal_effect_t *effect);

esp_err_t status_led_off(void);

bool status_led_effect_supported(const char *effect_id);

#ifdef __cplusplus
}
#endif

#endif
```

## Remove or Deprecate

Remove, or temporarily keep only as compatibility wrappers if required:

```c
status_led_state_t
status_led_pattern_t
status_led_signal_t

status_led_get_state()
status_led_set_state()
status_led_set_signal()
status_led_get_signal()
```

Preference: remove them if the project allows it.

If immediate removal breaks too many call sites, keep wrappers temporarily and mark them internally as legacy. All wrappers must eventually call `status_led_apply_effect()`.

## Minimum Effects to Implement in `status_led`

### `off`

- Turns the LED off.
- Ignores colors.
- Stops previous timers/animations.

### `solid`

- Turns on `color_a`.
- Uses `brightness`.
- Stops previous timers/animations.

### `blink`

- Alternates between `color_a` and off.
- Uses `time_a_ms` as on time.
- Uses `time_b_ms` as off time.
- If times are zero, use safe defaults, for example:
  - `time_a_ms = 250`
  - `time_b_ms = 750`

### `alternate`

- Alternates between `color_a` and `color_b`.
- Uses `time_a_ms` and `time_b_ms`.

### `breath`

- Smooth breathing effect using `color_a`.
- Uses `time_a_ms` as the period.
- If `time_a_ms == 0`, use a default such as `1200 ms`.

### `heartbeat`

- Two quick pulses using `color_a`, then a pause, then repeat.
- May use internal timings initially.
- `time_a_ms` may be used as the total period when provided.

### `pulse`

- Briefly turns on `color_a`, then turns off.
- Useful for very short direct driver tests.
- The official duration for semantic events must still be handled by `tele_indicator`.

## Internal Implementation

Create a dispatcher:

```c
esp_err_t status_led_apply_effect(const tele_signal_effect_t *effect)
{
    if (!effect) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(effect->id, "off") == 0) {
        return status_led_effect_off(effect);
    }

    if (strcmp(effect->id, "solid") == 0) {
        return status_led_effect_solid(effect);
    }

    if (strcmp(effect->id, "blink") == 0) {
        return status_led_effect_blink(effect);
    }

    if (strcmp(effect->id, "alternate") == 0) {
        return status_led_effect_alternate(effect);
    }

    if (strcmp(effect->id, "breath") == 0) {
        return status_led_effect_breath(effect);
    }

    if (strcmp(effect->id, "heartbeat") == 0) {
        return status_led_effect_heartbeat(effect);
    }

    if (strcmp(effect->id, "pulse") == 0) {
        return status_led_effect_pulse(effect);
    }

    return ESP_ERR_NOT_SUPPORTED;
}
```

---

# 6. Refactor `tele_indicator`

## Responsibility

`tele_indicator` must:

- register outputs;
- register sources;
- register events;
- keep active events;
- resolve priority;
- resolve priority ties;
- apply the effective effect to the correct output;
- automatically clear temporary events;
- allow reading the current effective state.

## Main Types

Update `tele_indicator.h`.

```c
#ifndef TELE_INDICATOR_H
#define TELE_INDICATOR_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "tele_signal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TELE_INDICATOR_ID_SIZE      32
#define TELE_INDICATOR_LABEL_SIZE   48

#define TELE_INDICATOR_MAX_EFFECT_IDS_PER_OUTPUT 12

typedef esp_err_t (*tele_indicator_output_apply_cb_t)(
    const tele_signal_effect_t *effect,
    void *ctx
);

typedef struct {
    const char *id;
    const char *label;

    const char *const *supported_effect_ids;
    uint8_t supported_effect_count;

    tele_indicator_output_apply_cb_t apply;
    void *ctx;
} tele_indicator_output_t;

typedef struct {
    const char *id;
    const char *label;
    uint8_t default_priority;
} tele_indicator_source_t;

typedef struct {
    const char *id;
    const char *label;

    const char *output_id;

    uint8_t priority;

    uint32_t duration_ms;

    tele_signal_effect_t effect;
} tele_indicator_event_t;

typedef struct {
    char source_id[TELE_INDICATOR_ID_SIZE];
    char event_id[TELE_INDICATOR_ID_SIZE];
    char output_id[TELE_INDICATOR_ID_SIZE];

    uint8_t priority;
    uint32_t sequence;

    bool active;

    tele_signal_effect_t effect;
} tele_indicator_effective_t;

esp_err_t tele_indicator_init(void);

esp_err_t tele_indicator_register_output(const tele_indicator_output_t *output);
esp_err_t tele_indicator_register_source(const tele_indicator_source_t *source);
esp_err_t tele_indicator_register_event(const tele_indicator_event_t *event);

esp_err_t tele_indicator_raise(const char *source_id,
                               const char *event_id);

esp_err_t tele_indicator_clear_source(const char *source_id);
esp_err_t tele_indicator_clear_event(const char *event_id);

esp_err_t tele_indicator_get_effective(tele_indicator_effective_t *out_effective);

#ifdef __cplusplus
}
#endif

#endif
```

## `reason` Field Decision

Do not include `reason` in this implementation.

The current code has a `reason` field, but it is apparently not consumed. It may be useful later for logs, serial debug, HTTP endpoints, MQTT telemetry, effective-state inspection, or automated tests. However, if it has no real use now, it increases API surface and internal buffers without delivering immediate value.

If needed later, reintroduce it through an extended API:

```c
esp_err_t tele_indicator_raise_ex(const char *source_id,
                                  const char *event_id,
                                  const char *reason);
```

For this implementation, keep the public API simple:

```c
esp_err_t tele_indicator_raise(const char *source_id,
                               const char *event_id);
```

---

# 7. Output Registry

## Concept

Each registered output represents a physical or logical destination.

Examples:

```text
status_led
front_leds
buzzer
display
```

Implement only `status_led` for now.

## `status_led` Output

In `app_indicators.c` or an equivalent application file:

```c
static esp_err_t app_status_led_output_apply(const tele_signal_effect_t *effect,
                                             void *ctx)
{
    (void)ctx;
    return status_led_apply_effect(effect);
}

static const char *const s_status_led_supported_effects[] = {
    "off",
    "solid",
    "blink",
    "alternate",
    "breath",
    "heartbeat",
    "pulse",
};

static const tele_indicator_output_t s_status_led_output = {
    .id = "status_led",
    .label = "Status LED",
    .supported_effect_ids = s_status_led_supported_effects,
    .supported_effect_count = sizeof(s_status_led_supported_effects) /
                              sizeof(s_status_led_supported_effects[0]),
    .apply = app_status_led_output_apply,
    .ctx = NULL,
};
```

`tele_indicator` must validate whether the registered event uses an effect supported by the selected output.

If the output does not support the effect, return:

```c
ESP_ERR_NOT_SUPPORTED
```

---

# 8. Source Registry

## Concept

A source represents the module requesting the indication.

Examples:

```text
system
wifi
product
battery
ota
capture
mqtt
```

## Example

```c
static const tele_indicator_source_t s_indicator_sources[] = {
    {
        .id = "system",
        .label = "System",
        .default_priority = 100,
    },
    {
        .id = "wifi",
        .label = "WiFi",
        .default_priority = 20,
    },
    {
        .id = "product",
        .label = "Product",
        .default_priority = 40,
    },
    {
        .id = "battery",
        .label = "Battery",
        .default_priority = 120,
    },
};
```

---

# 9. Event Registry

## Concept

An event defines:

- semantic ID;
- label;
- destination output;
- priority;
- duration;
- visual effect.

Duration belongs to the event, not to the effect.

## Persistent Events

Events with:

```c
.duration_ms = 0
```

must remain active until cleared.

## Temporary Events

Events with:

```c
.duration_ms > 0
```

must be cleared automatically by `tele_indicator`.

Example:

```c
.duration_ms = 1200
```

means the event stays active for 1.2 seconds and is then removed.

## Suggested Initial Events

```c
static const tele_indicator_event_t s_indicator_events[] = {
    {
        .id = "system.boot",
        .label = "Boot",
        .output_id = "status_led",
        .priority = 10,
        .duration_ms = 0,
        .effect = {
            .id = "breath",
            .color_a = { .red = 0, .green = 0, .blue = 255 },
            .time_a_ms = 1200,
            .time_b_ms = 0,
            .brightness = 64,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
    {
        .id = "wifi.connecting",
        .label = "WiFi connecting",
        .output_id = "status_led",
        .priority = 20,
        .duration_ms = 0,
        .effect = {
            .id = "blink",
            .color_a = { .red = 0, .green = 0, .blue = 255 },
            .time_a_ms = 250,
            .time_b_ms = 750,
            .brightness = 80,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
    {
        .id = "wifi.provisioning",
        .label = "WiFi provisioning",
        .output_id = "status_led",
        .priority = 30,
        .duration_ms = 0,
        .effect = {
            .id = "alternate",
            .color_a = { .red = 0, .green = 0, .blue = 255 },
            .color_b = { .red = 128, .green = 0, .blue = 255 },
            .time_a_ms = 400,
            .time_b_ms = 400,
            .brightness = 90,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
    {
        .id = "wifi.connected",
        .label = "WiFi connected",
        .output_id = "status_led",
        .priority = 15,
        .duration_ms = 1500,
        .effect = {
            .id = "solid",
            .color_a = { .red = 0, .green = 255, .blue = 0 },
            .brightness = 80,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
    {
        .id = "product.waiting",
        .label = "Product waiting",
        .output_id = "status_led",
        .priority = 35,
        .duration_ms = 0,
        .effect = {
            .id = "breath",
            .color_a = { .red = 255, .green = 180, .blue = 0 },
            .time_a_ms = 1400,
            .brightness = 70,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
    {
        .id = "product.transmitting",
        .label = "Product transmitting",
        .output_id = "status_led",
        .priority = 50,
        .duration_ms = 0,
        .effect = {
            .id = "blink",
            .color_a = { .red = 255, .green = 180, .blue = 0 },
            .time_a_ms = 120,
            .time_b_ms = 120,
            .brightness = 100,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
    {
        .id = "product.result_ok",
        .label = "Product result OK",
        .output_id = "status_led",
        .priority = 80,
        .duration_ms = 1200,
        .effect = {
            .id = "solid",
            .color_a = { .red = 0, .green = 255, .blue = 0 },
            .brightness = 100,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
    {
        .id = "product.result_alert",
        .label = "Product result alert",
        .output_id = "status_led",
        .priority = 90,
        .duration_ms = 2000,
        .effect = {
            .id = "blink",
            .color_a = { .red = 255, .green = 80, .blue = 0 },
            .time_a_ms = 180,
            .time_b_ms = 180,
            .brightness = 120,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
    {
        .id = "output.active",
        .label = "Output active",
        .output_id = "status_led",
        .priority = 70,
        .duration_ms = 0,
        .effect = {
            .id = "solid",
            .color_a = { .red = 255, .green = 255, .blue = 255 },
            .brightness = 80,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
    {
        .id = "battery.low",
        .label = "Low battery",
        .output_id = "status_led",
        .priority = 180,
        .duration_ms = 0,
        .effect = {
            .id = "heartbeat",
            .color_a = { .red = 255, .green = 120, .blue = 0 },
            .time_a_ms = 1800,
            .brightness = 100,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
    {
        .id = "system.error",
        .label = "System error",
        .output_id = "status_led",
        .priority = 250,
        .duration_ms = 0,
        .effect = {
            .id = "blink",
            .color_a = { .red = 255, .green = 0, .blue = 0 },
            .time_a_ms = 120,
            .time_b_ms = 120,
            .brightness = 120,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
};
```

---

# 10. Application Usage API

Application code must stop directly building colors, patterns, and effects for common semantic events.

Use:

```c
tele_indicator_raise("wifi", "wifi.connecting");
```

```c
tele_indicator_raise("wifi", "wifi.connected");
```

```c
tele_indicator_clear_source("wifi");
```

```c
tele_indicator_raise("system", "system.error");
```

```c
tele_indicator_raise("battery", "battery.low");
```

For direct driver tests only, application or test code may still call:

```c
status_led_apply_effect(&effect);
```

---

# 11. Effective Event Selection Policy

`tele_indicator` must select the effective event as follows:

1. consider only active events;
2. choose the event with the highest priority;
3. if priorities tie, choose the most recent event;
4. if no event is active, apply the `off` effect to the default output.

## Sequence

Maintain an internal counter:

```c
static uint32_t s_sequence;
```

Each `raise` increments `s_sequence`.

The active event receives the current sequence.

Priority tie example:

```c
if (candidate.priority > selected.priority) {
    selected = candidate;
} else if (candidate.priority == selected.priority &&
           candidate.sequence > selected.sequence) {
    selected = candidate;
}
```

---

# 12. Temporary Events

## Requirement

Events with `duration_ms > 0` must be cleared automatically.

## Suggested Implementation

Use `esp_timer`.

Each active event may have:

```c
esp_timer_handle_t clear_timer;
```

Alternatively, for a simpler or more compact implementation, use a single timer that reevaluates expiration based on timestamps.

For the first implementation, one `esp_timer` per active slot is acceptable.

## Expected Behavior

When `tele_indicator_raise()` is called:

- locate source;
- locate event;
- validate output;
- validate that the effect is supported by the output;
- activate the source slot;
- copy event/effect data;
- increment sequence;
- if `duration_ms > 0`, start the auto-clear timer;
- reevaluate effective state;
- apply output if the effective state changed.

When the timer expires:

- clear only that source event if it is still the same event and the same sequence;
- reevaluate effective state;
- apply output if needed.

This prevents an old timer from clearing a newer event.

---

# 13. Internal Active State

Internally, `tele_indicator` may work with one active slot per source.

```c
#define TELE_INDICATOR_MAX_SOURCES 12
#define TELE_INDICATOR_MAX_EVENTS  32
#define TELE_INDICATOR_MAX_OUTPUTS 8

typedef struct {
    bool active;

    char source_id[TELE_INDICATOR_ID_SIZE];
    char event_id[TELE_INDICATOR_ID_SIZE];
    char output_id[TELE_INDICATOR_ID_SIZE];

    uint8_t priority;
    uint32_t sequence;

    uint32_t duration_ms;

    tele_signal_effect_t effect;

    esp_timer_handle_t clear_timer;
} tele_indicator_active_slot_t;
```

---

# 14. Internal Copy of Registered Records

Avoid storing pointers to registration structs provided by the application, except for string literals with static lifetime.

Preference:

- copy `id`;
- copy `label`;
- copy `priority`;
- copy `effect`.

For simplicity and safety, internal slots should have their own buffers.

Avoid this:

```c
const tele_indicator_source_t *source;
```

Prefer this:

```c
tele_indicator_source_record_t source;
```

with internal copies of the fields.

---

# 15. Concurrency

`tele_indicator` must be safe for calls from multiple tasks.

Use an internal mutex:

```c
static SemaphoreHandle_t s_mutex;
```

Protect:

- registered records;
- active slots;
- effective state;
- sequence;
- timers.

Public functions must acquire the mutex.

Be careful not to hold the mutex while calling an output callback if that callback might cause reentry.

Recommended strategy:

1. acquire mutex;
2. modify internal state;
3. calculate the new effective state;
4. copy the output callback and effect to local variables;
5. release mutex;
6. call the output callback outside the mutex.

---

# 16. Effective State Comparison

Do not use direct `memcmp` on structs with padding.

Implement field-by-field comparison, or compare only what matters for output application:

- `output_id`;
- `effect.id`;
- `effect.color_a`;
- `effect.color_b`;
- `effect.time_a_ms`;
- `effect.time_b_ms`;
- `effect.brightness`;
- `effect.target_mask`;
- `effect.flags`.

Changing non-visual metadata must not reapply the LED if the visual effect is identical.

---

# 17. Default Output and `off` State

Define the default output:

```c
#define TELE_INDICATOR_DEFAULT_OUTPUT_ID "status_led"
```

When there is no active event:

- apply the `off` effect to the default output;
- mark effective state as inactive.

Off effect:

```c
static const tele_signal_effect_t s_off_effect = {
    .id = "off",
    .color_a = {0, 0, 0},
    .color_b = {0, 0, 0},
    .time_a_ms = 0,
    .time_b_ms = 0,
    .brightness = 0,
    .target_mask = TELE_SIGNAL_TARGET_ALL,
};
```

---

# 18. Application Indicator Registration

Create:

```text
main/app_indicators.h
main/app_indicators.c
```

## Header

```c
#ifndef APP_INDICATORS_H
#define APP_INDICATORS_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_indicators_start(void);

#ifdef __cplusplus
}
#endif

#endif
```

## Implementation

`app_indicators_start()` must:

1. start `status_led`;
2. initialize `tele_indicator`;
3. register the `status_led` output;
4. register sources;
5. register events;
6. optionally raise the initial `system.boot` event.

Example:

```c
esp_err_t app_indicators_start(void)
{
    ESP_RETURN_ON_ERROR(status_led_start(), TAG, "status_led_start failed");
    ESP_RETURN_ON_ERROR(tele_indicator_init(), TAG, "tele_indicator_init failed");

    ESP_RETURN_ON_ERROR(tele_indicator_register_output(&s_status_led_output),
                        TAG, "register status_led output failed");

    for (size_t i = 0; i < SOURCE_COUNT; ++i) {
        ESP_RETURN_ON_ERROR(tele_indicator_register_source(&s_indicator_sources[i]),
                            TAG, "register source failed");
    }

    for (size_t i = 0; i < EVENT_COUNT; ++i) {
        ESP_RETURN_ON_ERROR(tele_indicator_register_event(&s_indicator_events[i]),
                            TAG, "register event failed");
    }

    return tele_indicator_raise("system", "system.boot");
}
```

---

# 19. Migration from Existing Code

## Remove Duplications

Gradually eliminate:

```c
tele_indicator_pattern_t
tele_indicator_color_t
status_led_pattern_t
status_led_color_t
status_led_signal_t
```

Replace them with:

```c
tele_signal_color_t
tele_signal_effect_t
```

## Replace Old Calls

Before:

```c
status_led_set_state(STATUS_LED_STATE_WIFI_CONNECTING);
```

After:

```c
tele_indicator_raise("wifi", "wifi.connecting");
```

Before:

```c
status_led_set_signal(&signal);
```

After, when the intent is semantic:

```c
tele_indicator_raise("product", "product.transmitting");
```

After, when the intent is direct driver testing:

```c
status_led_apply_effect(&effect);
```

## Temporary Compatibility Adapter

Before the full `tele_indicator` refactor is complete, a temporary adapter may be used to keep the firmware functional while `status_led` already accepts `tele_signal_effect_t`.

Example temporary adapter:

```c
static esp_err_t connectivity_apply_indicator(const tele_indicator_state_t *state, void *ctx)
{
    (void)ctx;

    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    tele_signal_effect_t effect = {0};

    switch (state->pattern) {
    case TELE_INDICATOR_PATTERN_OFF:
        strncpy(effect.id, "off", sizeof(effect.id) - 1);
        break;

    case TELE_INDICATOR_PATTERN_SOLID:
        strncpy(effect.id, "solid", sizeof(effect.id) - 1);
        break;

    case TELE_INDICATOR_PATTERN_BREATH:
        strncpy(effect.id, "breath", sizeof(effect.id) - 1);
        effect.time_a_ms = 1200;
        break;

    case TELE_INDICATOR_PATTERN_BLINK_SLOW:
        strncpy(effect.id, "blink", sizeof(effect.id) - 1);
        effect.time_a_ms = 250;
        effect.time_b_ms = 750;
        break;

    case TELE_INDICATOR_PATTERN_BLINK_FAST:
        strncpy(effect.id, "blink", sizeof(effect.id) - 1);
        effect.time_a_ms = 120;
        effect.time_b_ms = 120;
        break;

    default:
        strncpy(effect.id, "off", sizeof(effect.id) - 1);
        break;
    }

    effect.color_a.red = state->color.red;
    effect.color_a.green = state->color.green;
    effect.color_a.blue = state->color.blue;
    effect.brightness = 100;
    effect.target_mask = TELE_SIGNAL_TARGET_ALL;

    return status_led_apply_effect(&effect);
}
```

This adapter is temporary and must be removed at the end of the refactor.

---

# 20. Minimum Tests

## `tele_indicator` Tests

Create host tests if the project already supports them.

Cover:

1. init without outputs;
2. output registration;
3. source registration;
4. event registration;
5. error when registering an event with an unknown output;
6. error when registering an event with an effect unsupported by the output;
7. persistent event raise;
8. clear source;
9. clear event;
10. selection by higher priority;
11. tie resolution by most recent event;
12. temporary event auto-clear;
13. old timer does not clear a newer event;
14. effective state does not reapply output if the visual effect did not change.

## `status_led` Tests

Cover at minimum:

1. `off`;
2. `solid`;
3. `blink`;
4. `alternate`;
5. `breath`;
6. unknown effect returns `ESP_ERR_NOT_SUPPORTED`.

---

# 21. Acceptance Criteria

The refactor is correct when:

- `status_led` no longer exposes semantic application states.
- `status_led` accepts `tele_signal_effect_t`.
- `tele_indicator` registers outputs, sources, and events.
- Events have `duration_ms`.
- Temporary events are cleared automatically.
- Higher priority wins.
- Priority ties are resolved by the most recent event.
- Application code can call:

```c
tele_indicator_raise("wifi", "wifi.connecting");
tele_indicator_clear_source("wifi");
```

- There is no longer a conversion between duplicated pattern enums.
- `status_led` does not include `tele_indicator.h`.
- `status_led` includes only `tele_signal.h`.
- `tele_indicator` includes `tele_signal.h`.
- The `status_led` output is registered in `app_indicators.c`.
- The system can initialize with `system.boot`.
- The `off` effect is applied when there is no active event.
- The `reason` field is not part of the initial implementation.

---

# 22. Out of Scope for This Stage

Do not implement now:

- complex multi-LED effects;
- scanning;
- fill;
- chase;
- rainbow;
- gradients;
- simultaneous indicator composition;
- multiple simultaneous effective events on the same output;
- color mixing by priority;
- effect scripting;
- dynamic configuration via JSON, MQTT, or NVS;
- manifest publishing;
- dynamic desktop UI rendering.

Only leave the structure ready to receive these resources later.

---

# 23. Architectural Note

The objective is not to create a heavy framework.

The objective is to have a simple, predictable, reusable base:

```text
tele_signal    -> describes what to show
status_led     -> knows how to show it on the physical LED
tele_indicator -> decides which event should be shown
app_indicators -> defines the product visual language
```

Keep the code small, static, and appropriate for ESP-IDF.

Use fixed arrays and clear limits.

Avoid unnecessary dynamic allocation.

Avoid abstraction beyond what is described in this plan.

---

# 24. Implementation Stages

Implementation must be done in small, verifiable stages.

Do not implement everything at once.

Each stage must compile before moving to the next one.

---

## Stage 1 — Create `tele_signal`

### Goal

Create the shared component with the types used by both `status_led` and `tele_indicator`.

### Tasks

- [ ] Create:

```text
components/tele_signal/
  include/tele_signal.h
  CMakeLists.txt
```

- [ ] Define:

```c
tele_signal_color_t
tele_signal_effect_t
```

- [ ] Define expected effect IDs:

```text
off
solid
blink
alternate
breath
heartbeat
pulse
```

- [ ] Create helper function:

```c
bool tele_signal_effect_id_is_valid(const char *id);
```

- [ ] Ensure the component compiles in isolation.

### Acceptance Criteria

- [ ] The project compiles with `tele_signal`.
- [ ] No existing behavior needs to change yet.
- [ ] No component depends on `tele_indicator.h` only to access visual types.

---

## Stage 2 — Refactor `status_led` to Use `tele_signal`

### Goal

Turn `status_led` into a physical LED driver with effects, without semantic application states.

### Tasks

- [ ] Change `status_led.h` to expose:

```c
esp_err_t status_led_start(void);
esp_err_t status_led_apply_effect(const tele_signal_effect_t *effect);
esp_err_t status_led_off(void);
bool status_led_effect_supported(const char *effect_id);
```

- [ ] Make `status_led` include:

```c
#include "tele_signal.h"
```

- [ ] Implement effect dispatcher for:

```text
off
solid
blink
alternate
breath
heartbeat
pulse
```

- [ ] Internally allow `blink` to reuse `alternate` logic.
- [ ] Internally allow `off` to reuse `solid` logic with zero brightness or an off color.
- [ ] Remove or isolate the old API:

```c
status_led_state_t
status_led_pattern_t
status_led_signal_t
status_led_set_state()
status_led_set_signal()
```

- [ ] If immediate removal breaks too many call sites, keep temporary wrappers and mark them as legacy.

### Acceptance Criteria

- [ ] `status_led` compiles without depending on `tele_indicator`.
- [ ] `status_led_apply_effect()` works for at least `off`, `solid`, and `blink`.
- [ ] Effects that are not yet refined may exist with simple implementations.
- [ ] States such as `WIFI_CONNECTING`, `LOW_BATTERY`, or `PRODUCT_RESULT_OK` are no longer the main responsibility of `status_led`.

---

## Stage 3 — Temporarily Adapt Current Code to the New `status_led`

### Goal

Keep firmware functional before the full `tele_indicator` refactor.

### Tasks

- [ ] Where the current code converts between `tele_indicator_pattern_t` and `status_led_pattern_t`, replace it with construction of `tele_signal_effect_t`.
- [ ] Use the temporary compatibility adapter from Section 19 if necessary.
- [ ] Keep this layer temporary.
- [ ] Remove it after the full refactor is complete.

### Acceptance Criteria

- [ ] Firmware remains functional.
- [ ] The LED still responds to old states.
- [ ] `status_led` already operates through the new `tele_signal_effect_t` API.

---

## Stage 4 — Refactor `tele_indicator` with Output Registry

### Goal

Make `tele_indicator` know registered outputs and the effects supported by each output.

### Tasks

- [ ] Update `tele_indicator.h` with:

```c
tele_indicator_output_t
tele_indicator_output_apply_cb_t
tele_indicator_register_output()
```

- [ ] Each output must contain:

```c
id
label
supported_effect_ids
supported_effect_count
apply
ctx
```

- [ ] Implement supported-effect validation.
- [ ] Return `ESP_ERR_NOT_SUPPORTED` when an event tries to use an effect unsupported by the selected output.
- [ ] Register the `status_led` output in the application.
- [ ] Make the `status_led` output callback call:

```c
status_led_apply_effect(effect);
```

### Acceptance Criteria

- [ ] `tele_indicator` can register at least one output.
- [ ] The `status_led` output is registered successfully.
- [ ] `tele_indicator` can apply an effect to `status_led`.
- [ ] Events with unsupported effects return an error.

---

## Stage 5 — Add Source and Event Registries

### Goal

Replace direct visual-state calls with semantic event-based calls.

### Tasks

- [ ] Implement:

```c
tele_indicator_register_source()
tele_indicator_register_event()
```

- [ ] Implement the event structure with:

```c
id
label
output_id
priority
duration_ms
effect
```

- [ ] Implement:

```c
tele_indicator_raise(source_id, event_id)
```

- [ ] Implement:

```c
tele_indicator_clear_source(source_id)
tele_indicator_clear_event(event_id)
```

- [ ] Create:

```text
main/app_indicators.c
main/app_indicators.h
```

- [ ] Move the product visual table to `app_indicators.c`.

### Acceptance Criteria

- [ ] Application code can call:

```c
tele_indicator_raise("wifi", "wifi.connecting");
```

- [ ] The LED applies the effect defined in the event registry.
- [ ] Application code no longer needs to manually build color, pattern, or effect for common events.

---

## Stage 6 — Implement Priority Policy

### Goal

Ensure multiple active events are arbitrated correctly.

### Policy

1. Higher priority wins.
2. If priority ties, the most recent event wins.
3. If there is no active event, apply `off`.

### Tasks

- [ ] Add internal counter:

```c
static uint32_t s_sequence;
```

- [ ] Each `raise` increments the sequence.
- [ ] Each active slot stores:

```c
priority
sequence
source_id
event_id
output_id
effect
```

- [ ] Implement effective-state selection.
- [ ] Avoid direct `memcmp` on structs.
- [ ] Compare only relevant visual fields to decide whether the output must be reapplied.

### Acceptance Criteria

- [ ] A higher-priority event replaces a lower-priority event.
- [ ] A lower-priority event appears again when the higher-priority event is cleared.
- [ ] If priorities tie, the most recent event wins.
- [ ] The output is not reapplied when the effective visual effect did not change.

---

## Stage 7 — Implement Automatic Event Duration

### Goal

Events with `duration_ms > 0` must clear automatically.

### Tasks

- [ ] Use `esp_timer`.
- [ ] Each active slot may have an automatic clear timer.
- [ ] When `raise` is called for a temporary event, start a timer if:

```c
duration_ms > 0
```

- [ ] When the timer expires:
  - [ ] verify that the source is still active;
  - [ ] verify that `event_id` is still the same;
  - [ ] verify that `sequence` is still the same;
  - [ ] clear the slot;
  - [ ] reevaluate effective state.
- [ ] Ensure this check prevents an old timer from clearing a new event.

### Acceptance Criteria

- [ ] Temporary event appears.
- [ ] After `duration_ms`, it is removed.
- [ ] The previous state returns automatically.
- [ ] An old timer does not clear a new event from the same source.

---

## Stage 8 — Add Mutex and Concurrency Safety

### Goal

Make `tele_indicator` safe for calls from multiple tasks.

### Tasks

- [ ] Create internal mutex:

```c
static SemaphoreHandle_t s_mutex;
```

- [ ] Protect:
  - [ ] registrations;
  - [ ] active slots;
  - [ ] effective state;
  - [ ] sequence;
  - [ ] timers.
- [ ] Do not call output callbacks while holding the mutex.
- [ ] Recommended strategy:
  - [ ] acquire mutex;
  - [ ] modify state;
  - [ ] calculate new effective state;
  - [ ] copy callback and effect to local variables;
  - [ ] release mutex;
  - [ ] call callback outside the mutex.

### Acceptance Criteria

- [ ] Concurrent calls do not corrupt internal state.
- [ ] Output callback does not run inside the critical section.
- [ ] There is no deadlock caused by accidental reentry.

---

## Stage 9 — Remove Legacy Layer

### Goal

Clean old code after the new architecture is working.

### Tasks

- [ ] Remove old conversions between:

```c
tele_indicator_pattern_t
status_led_pattern_t
```

- [ ] Remove old APIs if they still exist:

```c
status_led_set_state()
status_led_set_signal()
status_led_get_signal()
```

- [ ] Remove duplicated pattern/color enums.
- [ ] Update all application call sites to use:

```c
tele_indicator_raise()
tele_indicator_clear_source()
tele_indicator_clear_event()
```

- [ ] Remove `connectivity_apply_indicator()` if it has become only a legacy glue layer.

### Acceptance Criteria

- [ ] There is no duplicated pattern enum between `status_led` and `tele_indicator`.
- [ ] There is no 1:1 conversion between equivalent enums.
- [ ] The application uses semantic events.
- [ ] `status_led` remains independent from `tele_indicator`.

---

## Stage 10 — Tests and Hardware Validation

### Goal

Validate real behavior on ESP32-S3.

### Minimum Tests

- [ ] Boot:

```c
tele_indicator_raise("system", "system.boot");
```

Expected result: LED shows boot effect.

- [ ] Wi-Fi connecting:

```c
tele_indicator_raise("wifi", "wifi.connecting");
```

Expected result: LED blinks blue.

- [ ] Wi-Fi connected temporary event:

```c
tele_indicator_raise("wifi", "wifi.connected");
```

Expected result: LED turns green for `duration_ms`, then returns to the previous state or off.

- [ ] Product transmitting:

```c
tele_indicator_raise("product", "product.transmitting");
```

Expected result: transmission indication.

- [ ] Temporary OK result:

```c
tele_indicator_raise("product", "product.result_ok");
```

Expected result: temporary green indication and return to previous state.

- [ ] Error:

```c
tele_indicator_raise("system", "system.error");
```

Expected result: fast red blink, winning over every state except a future state with higher priority.

- [ ] Clear error:

```c
tele_indicator_clear_source("system");
```

Expected result: return to the next highest-priority active event or `off`.

- [ ] Low battery:

```c
tele_indicator_raise("battery", "battery.low");
```

Expected result: wins over normal Wi-Fi/product events.

### Acceptance Criteria

- [ ] Visual policy is predictable.
- [ ] Temporary events correctly return to the previous state.
- [ ] Higher-priority events win.
- [ ] The LED does not get stuck in an old effect.
- [ ] Firmware compiles without relevant warnings.

---

# 25. `reason` Field Decision

## Current Situation

The `reason` field exists in the current code, but it appears not to be consumed.

It may be useful in the future for:

- logs;
- serial debug;
- HTTP endpoints;
- MQTT telemetry;
- effective-state inspection;
- automated tests.

However, if there is no real use now, it increases API surface and internal buffers without delivering immediate value.

## Option A — Remove `reason` in This Stage

API becomes simpler:

```c
esp_err_t tele_indicator_raise(const char *source_id,
                               const char *event_id);
```

Effective state without `reason`:

```c
typedef struct {
    char source_id[TELE_INDICATOR_ID_SIZE];
    char event_id[TELE_INDICATOR_ID_SIZE];
    char output_id[TELE_INDICATOR_ID_SIZE];

    uint8_t priority;
    uint32_t sequence;

    bool active;

    tele_signal_effect_t effect;
} tele_indicator_effective_t;
```

Advantages:

- cleaner API;
- less string copying;
- less memory;
- less noise;
- one fewer field to test.

Disadvantage:

- loses potentially useful diagnostic information.

## Option B — Keep `reason`, but Make It Optional Later

Future API if needed:

```c
esp_err_t tele_indicator_raise_ex(const char *source_id,
                                  const char *event_id,
                                  const char *reason);

static inline esp_err_t tele_indicator_raise(const char *source_id,
                                             const char *event_id)
{
    return tele_indicator_raise_ex(source_id, event_id, NULL);
}
```

Advantages:

- keeps a path for debug;
- simple calls stay clean;
- preserves a future capability.

Disadvantage:

- still carries internal buffer and logic if implemented now.

## Recommendation for This Implementation

Use **Option A** initially.

Remove `reason` from the scope of this refactor.

If necessary in the future, reintroduce it as an extended API:

```c
tele_indicator_raise_ex()
```

Do not implement `reason` now only for “future possibility.”
