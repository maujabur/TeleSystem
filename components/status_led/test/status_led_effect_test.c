#include <assert.h>
#include <string.h>

#include "status_led.h"

int main(void)
{
    tele_signal_effect_t effect = {0};
    tele_signal_effect_t current = {0};
    const char *const *supported_effects = NULL;
    uint8_t supported_effect_count = 0;

    assert(status_led_start() == ESP_OK);

    supported_effects = status_led_supported_effect_ids();
    supported_effect_count = status_led_supported_effect_count();
    assert(supported_effects != NULL);
    assert(supported_effect_count == 7);
    assert(strcmp(supported_effects[0], "off") == 0);
    assert(strcmp(supported_effects[1], "solid") == 0);
    assert(strcmp(supported_effects[2], "blink") == 0);
    assert(strcmp(supported_effects[3], "alternate") == 0);
    assert(strcmp(supported_effects[4], "breath") == 0);
    assert(strcmp(supported_effects[5], "heartbeat") == 0);
    assert(strcmp(supported_effects[6], "pulse") == 0);

    assert(status_led_effect_supported("off"));
    assert(status_led_effect_supported("solid"));
    assert(status_led_effect_supported("blink"));
    assert(status_led_effect_supported("alternate"));
    assert(status_led_effect_supported("breath"));
    assert(status_led_effect_supported("heartbeat"));
    assert(status_led_effect_supported("pulse"));
    assert(!status_led_effect_supported("rainbow"));

    strcpy(effect.id, "solid");
    effect.color_a = (tele_signal_color_t) {.red = 10, .green = 20, .blue = 30};
    effect.brightness = 80;
    assert(status_led_apply_effect(&effect) == ESP_OK);
    assert(status_led_get_current_effect(&current) == ESP_OK);
    assert(strcmp(current.id, "solid") == 0);
    assert(current.color_a.green == 20);
    assert(current.brightness == 80);

    strcpy(effect.id, "blink");
    effect.time_a_ms = 120;
    effect.time_b_ms = 240;
    assert(status_led_apply_effect(&effect) == ESP_OK);
    assert(status_led_get_current_effect(&current) == ESP_OK);
    assert(strcmp(current.id, "blink") == 0);
    assert(current.time_a_ms == 120);
    assert(current.time_b_ms == 240);

    assert(status_led_off() == ESP_OK);
    assert(status_led_get_current_effect(&current) == ESP_OK);
    assert(strcmp(current.id, "off") == 0);

    strcpy(effect.id, "rainbow");
    assert(status_led_apply_effect(&effect) == ESP_ERR_NOT_SUPPORTED);
    assert(status_led_apply_effect(NULL) == ESP_ERR_INVALID_ARG);
    assert(status_led_get_current_effect(NULL) == ESP_ERR_INVALID_ARG);

    return 0;
}
