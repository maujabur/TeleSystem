#include "status_led.h"

#include <stddef.h>
#include <string.h>

static const char *const s_status_led_supported_effects[] = {
    TELE_SIGNAL_EFFECT_OFF,
    TELE_SIGNAL_EFFECT_SOLID,
    TELE_SIGNAL_EFFECT_BLINK,
    TELE_SIGNAL_EFFECT_ALTERNATE,
    TELE_SIGNAL_EFFECT_BREATH,
    TELE_SIGNAL_EFFECT_HEARTBEAT,
    TELE_SIGNAL_EFFECT_PULSE,
};

const char *const *status_led_supported_effect_ids(void)
{
    return s_status_led_supported_effects;
}

uint8_t status_led_supported_effect_count(void)
{
    return (uint8_t)(sizeof(s_status_led_supported_effects) /
                     sizeof(s_status_led_supported_effects[0]));
}

bool status_led_effect_supported(const char *effect_id)
{
    if (!effect_id || effect_id[0] == '\0') {
        return false;
    }

    for (uint8_t i = 0; i < status_led_supported_effect_count(); ++i) {
        if (strcmp(effect_id, s_status_led_supported_effects[i]) == 0) {
            return true;
        }
    }

    return false;
}
