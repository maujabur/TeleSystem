#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "tele_indicator.h"

static tele_indicator_state_t last_applied;
static int apply_calls;

static esp_err_t apply_sink(const tele_indicator_state_t *state, void *ctx)
{
    (void)ctx;
    assert(state != NULL);
    last_applied = *state;
    apply_calls++;
    return ESP_OK;
}

static const tele_indicator_source_t boot_source = {
    .id = "boot",
    .label = "Boot",
    .priority = 10,
};

static const tele_indicator_source_t wifi_source = {
    .id = "wifi",
    .label = "Wi-Fi",
    .priority = 30,
};

static const tele_indicator_source_t ota_source = {
    .id = "ota",
    .label = "OTA",
    .priority = 80,
};

int main(void)
{
    tele_indicator_state_t effective = {0};

    assert(tele_indicator_init(&(tele_indicator_config_t) {
        .apply = apply_sink,
    }) == ESP_OK);

    assert(tele_indicator_register_source(&boot_source) == ESP_OK);
    assert(tele_indicator_register_source(&wifi_source) == ESP_OK);
    assert(tele_indicator_register_source(&ota_source) == ESP_OK);
    assert(tele_indicator_register_source(&wifi_source) == ESP_ERR_INVALID_STATE);

    assert(tele_indicator_set_state(&(tele_indicator_state_t) {
        .source_id = "boot",
        .pattern = TELE_INDICATOR_PATTERN_BREATH,
        .color = {.red = 32, .green = 32, .blue = 32},
        .reason = "starting",
        .active = true,
    }) == ESP_OK);
    assert(apply_calls == 1);
    assert(strcmp(last_applied.source_id, "boot") == 0);
    assert(last_applied.pattern == TELE_INDICATOR_PATTERN_BREATH);
    assert(last_applied.color.red == 32);

    assert(tele_indicator_set_state(&(tele_indicator_state_t) {
        .source_id = "wifi",
        .pattern = TELE_INDICATOR_PATTERN_BLINK_FAST,
        .color = {.red = 0, .green = 64, .blue = 255},
        .reason = "connecting",
        .active = true,
    }) == ESP_OK);
    assert(apply_calls == 2);
    assert(strcmp(last_applied.source_id, "wifi") == 0);
    assert(last_applied.pattern == TELE_INDICATOR_PATTERN_BLINK_FAST);
    assert(last_applied.color.blue == 255);

    assert(tele_indicator_set_state(&(tele_indicator_state_t) {
        .source_id = "ota",
        .pattern = TELE_INDICATOR_PATTERN_BLINK_SLOW,
        .color = {.red = 128, .green = 0, .blue = 255},
        .reason = "updating",
        .active = true,
    }) == ESP_OK);
    assert(apply_calls == 3);
    assert(strcmp(last_applied.source_id, "ota") == 0);
    assert(last_applied.color.red == 128);

    assert(tele_indicator_get_effective(&effective) == ESP_OK);
    assert(strcmp(effective.source_id, "ota") == 0);
    assert(strcmp(effective.reason, "updating") == 0);

    assert(tele_indicator_clear_state("ota") == ESP_OK);
    assert(apply_calls == 4);
    assert(strcmp(last_applied.source_id, "wifi") == 0);
    assert(strcmp(last_applied.reason, "connecting") == 0);

    assert(tele_indicator_clear_state("wifi") == ESP_OK);
    assert(apply_calls == 5);
    assert(strcmp(last_applied.source_id, "boot") == 0);

    assert(tele_indicator_clear_state("boot") == ESP_OK);
    assert(apply_calls == 6);
    assert(last_applied.pattern == TELE_INDICATOR_PATTERN_OFF);
    assert(!last_applied.active);

    assert(tele_indicator_set_state(&(tele_indicator_state_t) {
        .source_id = "missing",
        .pattern = TELE_INDICATOR_PATTERN_SOLID,
        .active = true,
    }) == ESP_ERR_NOT_FOUND);

    return 0;
}
