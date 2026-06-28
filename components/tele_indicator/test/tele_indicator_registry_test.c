#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "tele_indicator.h"

static tele_signal_effect_t last_effect;
static int apply_calls;

static esp_err_t apply_sink(const tele_signal_effect_t *effect, void *ctx)
{
    (void)ctx;
    assert(effect != NULL);
    last_effect = *effect;
    apply_calls++;
    return ESP_OK;
}

static const char *const status_led_effects[] = {
    "off",
    "solid",
    "blink",
    "alternate",
    "breath",
    "heartbeat",
    "pulse",
};

static const tele_indicator_output_t status_led_output = {
    .id = "status_led",
    .supported_effect_ids = status_led_effects,
    .supported_effect_count = sizeof(status_led_effects) / sizeof(status_led_effects[0]),
    .apply = apply_sink,
};

static const tele_indicator_output_t limited_output = {
    .id = "limited",
    .supported_effect_ids = status_led_effects,
    .supported_effect_count = 1,
    .apply = apply_sink,
};

static const tele_indicator_source_t system_source = {
    .id = "system",
    .default_priority = 100,
};

static const tele_indicator_source_t wifi_source = {
    .id = "wifi",
    .default_priority = 30,
};

static tele_indicator_event_t make_event(const char *id,
                                         const char *output_id,
                                         const char *effect_id,
                                         uint8_t priority,
                                         uint32_t duration_ms,
                                         uint8_t red)
{
    tele_indicator_event_t event = {
        .id = id,
        .output_id = output_id,
        .priority = priority,
        .duration_ms = duration_ms,
        .effect = {
            .color_a = {.red = red},
            .brightness = 80,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    };
    strncpy(event.effect.id, effect_id, sizeof(event.effect.id) - 1);
    return event;
}

int main(void)
{
    tele_indicator_effective_t effective = {0};

    assert(tele_indicator_init() == ESP_OK);
    assert(tele_indicator_register_output(&status_led_output) == ESP_OK);
    assert(tele_indicator_register_output(&status_led_output) == ESP_ERR_INVALID_STATE);
    assert(tele_indicator_register_source(&system_source) == ESP_OK);
    assert(tele_indicator_register_source(&wifi_source) == ESP_OK);

    assert(tele_indicator_register_event(&(tele_indicator_event_t) {
        .id = "missing.output",
        .output_id = "missing",
        .priority = 10,
        .effect = {.id = "solid"},
    }) == ESP_ERR_NOT_FOUND);

    assert(tele_indicator_register_output(&limited_output) == ESP_OK);
    assert(tele_indicator_register_event(&(tele_indicator_event_t) {
        .id = "unsupported.effect",
        .output_id = "limited",
        .priority = 10,
        .effect = {.id = "solid"},
    }) == ESP_ERR_NOT_SUPPORTED);

    tele_indicator_event_t boot = make_event("system.boot", "status_led", "breath", 10, 0, 20);
    tele_indicator_event_t wifi_connecting = make_event("wifi.connecting", "status_led", "blink", 30, 0, 40);
    tele_indicator_event_t wifi_connected = make_event("wifi.connected", "status_led", "solid", 15, 1000, 60);
    tele_indicator_event_t system_error = make_event("system.error", "status_led", "blink", 250, 0, 255);
    tele_indicator_event_t wifi_same_visual = make_event("wifi.same_visual", "status_led", "blink", 30, 0, 40);

    assert(tele_indicator_register_event(&boot) == ESP_OK);
    assert(tele_indicator_register_event(&wifi_connecting) == ESP_OK);
    assert(tele_indicator_register_event(&wifi_connected) == ESP_OK);
    assert(tele_indicator_register_event(&system_error) == ESP_OK);
    assert(tele_indicator_register_event(&wifi_same_visual) == ESP_OK);

    assert(tele_indicator_raise("system", "system.boot") == ESP_OK);
    assert(apply_calls == 1);
    assert(strcmp(last_effect.id, "breath") == 0);

    assert(tele_indicator_raise("wifi", "wifi.connecting") == ESP_OK);
    assert(apply_calls == 2);
    assert(strcmp(last_effect.id, "blink") == 0);
    assert(last_effect.color_a.red == 40);

    assert(tele_indicator_raise("system", "system.error") == ESP_OK);
    assert(apply_calls == 3);
    assert(last_effect.color_a.red == 255);

    assert(tele_indicator_clear_event("system.error") == ESP_OK);
    assert(apply_calls == 4);
    assert(last_effect.color_a.red == 40);

    assert(tele_indicator_raise("system", "system.boot") == ESP_OK);
    assert(apply_calls == 4);

    assert(tele_indicator_raise("wifi", "wifi.same_visual") == ESP_OK);
    assert(apply_calls == 4);
    assert(tele_indicator_get_effective(&effective) == ESP_OK);
    assert(strcmp(effective.event_id, "wifi.same_visual") == 0);

    assert(tele_indicator_raise("wifi", "wifi.connected") == ESP_OK);
    assert(apply_calls == 5);
    assert(strcmp(last_effect.id, "solid") == 0);
    assert(last_effect.color_a.red == 60);

    tele_indicator_host_advance_time(999);
    assert(tele_indicator_get_effective(&effective) == ESP_OK);
    assert(strcmp(effective.event_id, "wifi.connected") == 0);

    tele_indicator_host_advance_time(1);
    assert(tele_indicator_get_effective(&effective) == ESP_OK);
    assert(strcmp(effective.event_id, "system.boot") == 0);
    assert(apply_calls == 6);

    assert(tele_indicator_raise("wifi", "wifi.connected") == ESP_OK);
    assert(apply_calls == 7);
    tele_indicator_host_advance_time(500);
    assert(tele_indicator_raise("wifi", "wifi.connecting") == ESP_OK);
    assert(apply_calls == 8);
    tele_indicator_host_advance_time(500);
    assert(tele_indicator_get_effective(&effective) == ESP_OK);
    assert(strcmp(effective.event_id, "wifi.connecting") == 0);
    assert(apply_calls == 8);
    assert(tele_indicator_clear_source("wifi") == ESP_OK);
    assert(apply_calls == 9);

    assert(tele_indicator_clear_source("system") == ESP_OK);
    assert(apply_calls == 10);
    assert(strcmp(last_effect.id, "off") == 0);
    assert(tele_indicator_get_effective(&effective) == ESP_OK);
    assert(!effective.active);

    assert(tele_indicator_raise("missing", "system.boot") == ESP_ERR_NOT_FOUND);
    assert(tele_indicator_raise("system", "missing.event") == ESP_ERR_NOT_FOUND);

    return 0;
}
