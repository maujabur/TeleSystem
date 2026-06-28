#include "tele_signal.h"

#include <string.h>

bool tele_signal_effect_id_is_valid(const char *id)
{
    static const char *const supported_ids[] = {
        TELE_SIGNAL_EFFECT_OFF,
        TELE_SIGNAL_EFFECT_SOLID,
        TELE_SIGNAL_EFFECT_BLINK,
        TELE_SIGNAL_EFFECT_ALTERNATE,
        TELE_SIGNAL_EFFECT_BREATH,
        TELE_SIGNAL_EFFECT_HEARTBEAT,
        TELE_SIGNAL_EFFECT_PULSE,
    };

    if (!id || id[0] == '\0') {
        return false;
    }

    for (size_t i = 0; i < sizeof(supported_ids) / sizeof(supported_ids[0]); ++i) {
        if (strcmp(id, supported_ids[i]) == 0) {
            return true;
        }
    }

    return false;
}
