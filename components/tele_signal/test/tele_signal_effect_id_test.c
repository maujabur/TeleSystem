#include <assert.h>
#include <stddef.h>

#include "tele_signal.h"

int main(void)
{
    assert(tele_signal_effect_id_is_valid("off"));
    assert(tele_signal_effect_id_is_valid("solid"));
    assert(tele_signal_effect_id_is_valid("blink"));
    assert(tele_signal_effect_id_is_valid("alternate"));
    assert(tele_signal_effect_id_is_valid("breath"));
    assert(tele_signal_effect_id_is_valid("heartbeat"));
    assert(tele_signal_effect_id_is_valid("pulse"));

    assert(!tele_signal_effect_id_is_valid(NULL));
    assert(!tele_signal_effect_id_is_valid(""));
    assert(!tele_signal_effect_id_is_valid("rainbow"));

    return 0;
}
