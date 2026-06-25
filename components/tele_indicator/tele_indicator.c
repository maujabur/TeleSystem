#include "tele_indicator.h"

#include <string.h>

#define TELE_INDICATOR_MAX_SOURCES 12

typedef struct {
    const tele_indicator_source_t *source;
    tele_indicator_state_t state;
} tele_indicator_slot_t;

static tele_indicator_slot_t s_slots[TELE_INDICATOR_MAX_SOURCES];
static size_t s_slot_count;
static tele_indicator_apply_cb_t s_apply;
static void *s_apply_ctx;
static tele_indicator_state_t s_effective;
static bool s_initialized;

static bool text_valid(const char *text)
{
    return text && text[0] != '\0';
}

static bool copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return false;
    }

    dst[0] = '\0';
    if (!src) {
        return true;
    }

    size_t len = strlen(src);
    if (len >= dst_size) {
        return false;
    }

    memcpy(dst, src, len + 1);
    return true;
}

static int find_slot(const char *source_id)
{
    if (!text_valid(source_id)) {
        return -1;
    }

    for (size_t i = 0; i < s_slot_count; ++i) {
        if (strcmp(s_slots[i].source->id, source_id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static tele_indicator_state_t off_state(void)
{
    return (tele_indicator_state_t) {
        .pattern = TELE_INDICATOR_PATTERN_OFF,
        .active = false,
    };
}

static bool states_equal(const tele_indicator_state_t *a,
                         const tele_indicator_state_t *b)
{
    return a && b && memcmp(a, b, sizeof(*a)) == 0;
}

static esp_err_t apply_effective_if_changed(void)
{
    tele_indicator_state_t selected = off_state();
    int selected_index = -1;

    for (size_t i = 0; i < s_slot_count; ++i) {
        if (!s_slots[i].state.active) {
            continue;
        }
        if (selected_index < 0 ||
            s_slots[i].source->priority > s_slots[selected_index].source->priority) {
            selected_index = (int)i;
            selected = s_slots[i].state;
        }
    }

    if (states_equal(&selected, &s_effective)) {
        return ESP_OK;
    }

    s_effective = selected;
    return s_apply ? s_apply(&s_effective, s_apply_ctx) : ESP_OK;
}

esp_err_t tele_indicator_init(const tele_indicator_config_t *config)
{
    if (!config || !config->apply) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(s_slots, 0, sizeof(s_slots));
    s_slot_count = 0;
    s_apply = config->apply;
    s_apply_ctx = config->ctx;
    s_effective = off_state();
    s_initialized = true;
    return ESP_OK;
}

esp_err_t tele_indicator_register_source(const tele_indicator_source_t *source)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!source || !text_valid(source->id)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (find_slot(source->id) >= 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_slot_count >= TELE_INDICATOR_MAX_SOURCES) {
        return ESP_ERR_NO_MEM;
    }

    s_slots[s_slot_count++] = (tele_indicator_slot_t) {
        .source = source,
    };
    return ESP_OK;
}

esp_err_t tele_indicator_set_state(const tele_indicator_state_t *state)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!state || !text_valid(state->source_id)) {
        return ESP_ERR_INVALID_ARG;
    }

    int index = find_slot(state->source_id);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    tele_indicator_state_t normalized = *state;
    if (!copy_text(normalized.source_id, sizeof(normalized.source_id), state->source_id) ||
        !copy_text(normalized.reason, sizeof(normalized.reason), state->reason)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_slots[index].state = normalized;
    return apply_effective_if_changed();
}

esp_err_t tele_indicator_clear_state(const char *source_id)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    int index = find_slot(source_id);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    s_slots[index].state = off_state();
    return apply_effective_if_changed();
}

esp_err_t tele_indicator_get_effective(tele_indicator_state_t *out_state)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!out_state) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_state = s_effective;
    return ESP_OK;
}

const char *tele_indicator_pattern_name(tele_indicator_pattern_t pattern)
{
    switch (pattern) {
    case TELE_INDICATOR_PATTERN_OFF:
        return "off";
    case TELE_INDICATOR_PATTERN_SOLID:
        return "solid";
    case TELE_INDICATOR_PATTERN_BREATH:
        return "breath";
    case TELE_INDICATOR_PATTERN_BLINK_SLOW:
        return "blink_slow";
    case TELE_INDICATOR_PATTERN_BLINK_FAST:
        return "blink_fast";
    default:
        return "unknown";
    }
}
