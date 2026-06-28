#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TELE_CHANNEL_FLAG_MQTT = 1U << 0,
    TELE_CHANNEL_FLAG_WEB = 1U << 1,
    TELE_CHANNEL_FLAG_SERIAL = 1U << 2,
    TELE_CHANNEL_FLAG_LORA = 1U << 3,
} tele_channel_flags_t;

#ifdef __cplusplus
}
#endif
