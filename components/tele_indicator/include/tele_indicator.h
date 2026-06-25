#ifndef TELE_INDICATOR_H
#define TELE_INDICATOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef TELE_INDICATOR_HOST_TEST
#include "tele_indicator_host_stubs.h"
#else
#include "esp_err.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define TELE_INDICATOR_SOURCE_ID_SIZE 32
#define TELE_INDICATOR_LABEL_SIZE 48
#define TELE_INDICATOR_REASON_SIZE 48

typedef enum {
    TELE_INDICATOR_PATTERN_OFF = 0,
    TELE_INDICATOR_PATTERN_SOLID,
    TELE_INDICATOR_PATTERN_BREATH,
    TELE_INDICATOR_PATTERN_BLINK_SLOW,
    TELE_INDICATOR_PATTERN_BLINK_FAST,
} tele_indicator_pattern_t;

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} tele_indicator_color_t;

typedef struct {
    const char *id;
    const char *label;
    uint8_t priority;
} tele_indicator_source_t;

typedef struct {
    char source_id[TELE_INDICATOR_SOURCE_ID_SIZE];
    tele_indicator_pattern_t pattern;
    tele_indicator_color_t color;
    char reason[TELE_INDICATOR_REASON_SIZE];
    bool active;
} tele_indicator_state_t;

typedef esp_err_t (*tele_indicator_apply_cb_t)(const tele_indicator_state_t *state,
                                               void *ctx);

typedef struct {
    tele_indicator_apply_cb_t apply;
    void *ctx;
} tele_indicator_config_t;

esp_err_t tele_indicator_init(const tele_indicator_config_t *config);
esp_err_t tele_indicator_register_source(const tele_indicator_source_t *source);
esp_err_t tele_indicator_set_state(const tele_indicator_state_t *state);
esp_err_t tele_indicator_clear_state(const char *source_id);
esp_err_t tele_indicator_get_effective(tele_indicator_state_t *out_state);

const char *tele_indicator_pattern_name(tele_indicator_pattern_t pattern);

#ifdef __cplusplus
}
#endif

#endif
