#ifndef TELE_INDICATOR_H
#define TELE_INDICATOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef TELE_INDICATOR_HOST_TEST
#include "tele_indicator_host_stubs.h"
#else
#include "esp_err.h"
#endif

#include "tele_signal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TELE_INDICATOR_ID_SIZE 32
#define TELE_INDICATOR_MAX_EFFECT_IDS_PER_OUTPUT 12
#define TELE_INDICATOR_DEFAULT_OUTPUT_ID "status_led"

typedef esp_err_t (*tele_indicator_output_apply_cb_t)(
    const tele_signal_effect_t *effect,
    void *ctx
);

typedef struct {
    const char *id;

    const char *const *supported_effect_ids;
    uint8_t supported_effect_count;

    tele_indicator_output_apply_cb_t apply;
    void *ctx;
} tele_indicator_output_t;

typedef struct {
    const char *id;
    uint8_t default_priority;
} tele_indicator_source_t;

typedef struct {
    const char *id;

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

#ifdef TELE_INDICATOR_HOST_TEST
void tele_indicator_host_advance_time(uint32_t elapsed_ms);
#endif

#ifdef __cplusplus
}
#endif

#endif
