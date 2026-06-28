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

#define TELE_SIGNAL_EFFECT_OFF       "off"
#define TELE_SIGNAL_EFFECT_SOLID     "solid"
#define TELE_SIGNAL_EFFECT_BLINK     "blink"
#define TELE_SIGNAL_EFFECT_ALTERNATE "alternate"
#define TELE_SIGNAL_EFFECT_BREATH    "breath"
#define TELE_SIGNAL_EFFECT_HEARTBEAT "heartbeat"
#define TELE_SIGNAL_EFFECT_PULSE     "pulse"

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
