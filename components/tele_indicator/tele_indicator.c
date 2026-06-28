#include "tele_indicator.h"

#include <string.h>

#ifndef TELE_INDICATOR_HOST_TEST
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

#define TELE_INDICATOR_MAX_OUTPUTS 8
#define TELE_INDICATOR_MAX_SOURCES 12
#define TELE_INDICATOR_MAX_EVENTS 32

typedef struct {
    char id[TELE_INDICATOR_ID_SIZE];
    char supported_effect_ids[TELE_INDICATOR_MAX_EFFECT_IDS_PER_OUTPUT][TELE_SIGNAL_ID_SIZE];
    uint8_t supported_effect_count;
    tele_indicator_output_apply_cb_t apply;
    void *ctx;
} tele_indicator_output_record_t;

typedef struct {
    char id[TELE_INDICATOR_ID_SIZE];
    uint8_t default_priority;
} tele_indicator_source_record_t;

typedef struct {
    char id[TELE_INDICATOR_ID_SIZE];
    char output_id[TELE_INDICATOR_ID_SIZE];
    uint8_t priority;
    uint32_t duration_ms;
    tele_signal_effect_t effect;
} tele_indicator_event_record_t;

typedef struct {
    bool active;
    char source_id[TELE_INDICATOR_ID_SIZE];
    char event_id[TELE_INDICATOR_ID_SIZE];
    char output_id[TELE_INDICATOR_ID_SIZE];
    uint8_t priority;
    uint32_t sequence;
    uint32_t duration_ms;
    uint64_t expires_at_ms;
    tele_signal_effect_t effect;
#ifndef TELE_INDICATOR_HOST_TEST
    esp_timer_handle_t clear_timer;
#endif
} tele_indicator_active_slot_t;

static tele_indicator_output_record_t s_outputs[TELE_INDICATOR_MAX_OUTPUTS];
static size_t s_output_count;
static tele_indicator_source_record_t s_sources[TELE_INDICATOR_MAX_SOURCES];
static size_t s_source_count;
static tele_indicator_event_record_t s_events[TELE_INDICATOR_MAX_EVENTS];
static size_t s_event_count;
static tele_indicator_active_slot_t s_active[TELE_INDICATOR_MAX_SOURCES];
static tele_indicator_effective_t s_effective;
static uint32_t s_sequence;
static bool s_initialized;

#ifdef TELE_INDICATOR_HOST_TEST
static uint64_t s_now_ms;
#else
static SemaphoreHandle_t s_mutex;
#endif

static const tele_signal_effect_t s_off_effect = {
    .id = TELE_SIGNAL_EFFECT_OFF,
    .target_mask = TELE_SIGNAL_TARGET_ALL,
};

static bool text_valid(const char *text)
{
    return text && text[0] != '\0';
}

static bool copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0 || !src) {
        return false;
    }

    size_t len = strlen(src);
    if (len >= dst_size) {
        return false;
    }

    memcpy(dst, src, len + 1);
    return true;
}

static uint64_t now_ms(void)
{
#ifdef TELE_INDICATOR_HOST_TEST
    return s_now_ms;
#else
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
#endif
}

static void lock_indicator(void)
{
#ifndef TELE_INDICATOR_HOST_TEST
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
#endif
}

static void unlock_indicator(void)
{
#ifndef TELE_INDICATOR_HOST_TEST
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
#endif
}

static int find_output(const char *id)
{
    if (!text_valid(id)) {
        return -1;
    }

    for (size_t i = 0; i < s_output_count; ++i) {
        if (strcmp(s_outputs[i].id, id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int find_source(const char *id)
{
    if (!text_valid(id)) {
        return -1;
    }

    for (size_t i = 0; i < s_source_count; ++i) {
        if (strcmp(s_sources[i].id, id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int find_event(const char *id)
{
    if (!text_valid(id)) {
        return -1;
    }

    for (size_t i = 0; i < s_event_count; ++i) {
        if (strcmp(s_events[i].id, id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool output_supports_effect(const tele_indicator_output_record_t *output,
                                   const char *effect_id)
{
    if (!output || !text_valid(effect_id)) {
        return false;
    }

    for (uint8_t i = 0; i < output->supported_effect_count; ++i) {
        if (strcmp(output->supported_effect_ids[i], effect_id) == 0) {
            return true;
        }
    }

    return false;
}

static bool colors_equal(tele_signal_color_t a, tele_signal_color_t b)
{
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

static bool visual_effects_equal(const tele_indicator_effective_t *a,
                                 const tele_indicator_effective_t *b)
{
    if (!a || !b) {
        return false;
    }

    return strcmp(a->output_id, b->output_id) == 0 &&
           strcmp(a->effect.id, b->effect.id) == 0 &&
           colors_equal(a->effect.color_a, b->effect.color_a) &&
           colors_equal(a->effect.color_b, b->effect.color_b) &&
           a->effect.time_a_ms == b->effect.time_a_ms &&
           a->effect.time_b_ms == b->effect.time_b_ms &&
           a->effect.brightness == b->effect.brightness &&
           a->effect.target_mask == b->effect.target_mask &&
           a->effect.flags == b->effect.flags &&
           a->active == b->active;
}

static tele_indicator_effective_t inactive_effective(void)
{
    tele_indicator_effective_t effective = {
        .active = false,
        .effect = s_off_effect,
    };
    copy_text(effective.output_id, sizeof(effective.output_id), TELE_INDICATOR_DEFAULT_OUTPUT_ID);
    return effective;
}

static tele_indicator_effective_t active_to_effective(const tele_indicator_active_slot_t *slot)
{
    tele_indicator_effective_t effective = {
        .priority = slot->priority,
        .sequence = slot->sequence,
        .active = true,
        .effect = slot->effect,
    };
    copy_text(effective.source_id, sizeof(effective.source_id), slot->source_id);
    copy_text(effective.event_id, sizeof(effective.event_id), slot->event_id);
    copy_text(effective.output_id, sizeof(effective.output_id), slot->output_id);
    return effective;
}

static tele_indicator_effective_t select_effective_locked(void)
{
    const tele_indicator_active_slot_t *selected = NULL;

    for (size_t i = 0; i < s_source_count; ++i) {
        const tele_indicator_active_slot_t *candidate = &s_active[i];
        if (!candidate->active) {
            continue;
        }
        if (!selected ||
            candidate->priority > selected->priority ||
            (candidate->priority == selected->priority &&
             candidate->sequence > selected->sequence)) {
            selected = candidate;
        }
    }

    return selected ? active_to_effective(selected) : inactive_effective();
}

static void clear_slot_locked(size_t index)
{
    if (index >= TELE_INDICATOR_MAX_SOURCES) {
        return;
    }

#ifndef TELE_INDICATOR_HOST_TEST
    esp_timer_handle_t timer = s_active[index].clear_timer;
    if (s_active[index].clear_timer) {
        esp_timer_stop(s_active[index].clear_timer);
    }
#endif
    memset(&s_active[index], 0, sizeof(s_active[index]));
#ifndef TELE_INDICATOR_HOST_TEST
    s_active[index].clear_timer = timer;
#endif
}

static bool prepare_apply_locked(tele_signal_effect_t *out_effect,
                                 tele_indicator_output_apply_cb_t *out_apply,
                                 void **out_ctx)
{
    tele_indicator_effective_t selected = select_effective_locked();
    bool visual_changed = !visual_effects_equal(&selected, &s_effective);
    int output_index = find_output(selected.output_id);

    s_effective = selected;

    if (!visual_changed || output_index < 0 || !s_outputs[output_index].apply) {
        return false;
    }

    *out_effect = selected.effect;
    *out_apply = s_outputs[output_index].apply;
    *out_ctx = s_outputs[output_index].ctx;
    return true;
}

static esp_err_t apply_after_unlock(tele_signal_effect_t *effect,
                                    tele_indicator_output_apply_cb_t apply,
                                    void *ctx,
                                    bool should_apply)
{
    if (!should_apply) {
        return ESP_OK;
    }
    return apply(effect, ctx);
}

#ifndef TELE_INDICATOR_HOST_TEST
static void clear_timer_cb(void *arg)
{
    size_t index = (size_t)arg;
    tele_signal_effect_t effect = {0};
    tele_indicator_output_apply_cb_t apply = NULL;
    void *ctx = NULL;
    bool should_apply = false;

    lock_indicator();
    if (index < s_source_count &&
        s_active[index].active &&
        s_active[index].duration_ms > 0 &&
        s_active[index].expires_at_ms <= now_ms()) {
        clear_slot_locked(index);
        should_apply = prepare_apply_locked(&effect, &apply, &ctx);
    }
    unlock_indicator();

    (void)apply_after_unlock(&effect, apply, ctx, should_apply);
}
#endif

esp_err_t tele_indicator_init(void)
{
    memset(s_outputs, 0, sizeof(s_outputs));
    memset(s_sources, 0, sizeof(s_sources));
    memset(s_events, 0, sizeof(s_events));
    memset(s_active, 0, sizeof(s_active));
    s_output_count = 0;
    s_source_count = 0;
    s_event_count = 0;
    s_sequence = 0;
    s_effective = inactive_effective();
#ifdef TELE_INDICATOR_HOST_TEST
    s_now_ms = 0;
#else
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
#endif
    s_initialized = true;
    return ESP_OK;
}

esp_err_t tele_indicator_register_output(const tele_indicator_output_t *output)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!output || !text_valid(output->id) || !output->apply ||
        !output->supported_effect_ids ||
        output->supported_effect_count == 0 ||
        output->supported_effect_count > TELE_INDICATOR_MAX_EFFECT_IDS_PER_OUTPUT) {
        return ESP_ERR_INVALID_ARG;
    }

    lock_indicator();
    if (find_output(output->id) >= 0) {
        unlock_indicator();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_output_count >= TELE_INDICATOR_MAX_OUTPUTS) {
        unlock_indicator();
        return ESP_ERR_NO_MEM;
    }

    tele_indicator_output_record_t *record = &s_outputs[s_output_count];
    memset(record, 0, sizeof(*record));
    if (!copy_text(record->id, sizeof(record->id), output->id)) {
        unlock_indicator();
        return ESP_ERR_INVALID_ARG;
    }

    for (uint8_t i = 0; i < output->supported_effect_count; ++i) {
        if (!tele_signal_effect_id_is_valid(output->supported_effect_ids[i]) ||
            !copy_text(record->supported_effect_ids[i],
                       sizeof(record->supported_effect_ids[i]),
                       output->supported_effect_ids[i])) {
            unlock_indicator();
            return ESP_ERR_INVALID_ARG;
        }
    }

    record->supported_effect_count = output->supported_effect_count;
    record->apply = output->apply;
    record->ctx = output->ctx;
    s_output_count++;
    unlock_indicator();
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

    lock_indicator();
    if (find_source(source->id) >= 0) {
        unlock_indicator();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_source_count >= TELE_INDICATOR_MAX_SOURCES) {
        unlock_indicator();
        return ESP_ERR_NO_MEM;
    }

    tele_indicator_source_record_t *record = &s_sources[s_source_count];
    memset(record, 0, sizeof(*record));
    if (!copy_text(record->id, sizeof(record->id), source->id)) {
        unlock_indicator();
        return ESP_ERR_INVALID_ARG;
    }
    record->default_priority = source->default_priority;
    s_source_count++;
    unlock_indicator();
    return ESP_OK;
}

esp_err_t tele_indicator_register_event(const tele_indicator_event_t *event)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!event || !text_valid(event->id) || !text_valid(event->output_id) ||
        !tele_signal_effect_id_is_valid(event->effect.id)) {
        return ESP_ERR_INVALID_ARG;
    }

    lock_indicator();
    if (find_event(event->id) >= 0) {
        unlock_indicator();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_event_count >= TELE_INDICATOR_MAX_EVENTS) {
        unlock_indicator();
        return ESP_ERR_NO_MEM;
    }

    int output_index = find_output(event->output_id);
    if (output_index < 0) {
        unlock_indicator();
        return ESP_ERR_NOT_FOUND;
    }
    if (!output_supports_effect(&s_outputs[output_index], event->effect.id)) {
        unlock_indicator();
        return ESP_ERR_NOT_SUPPORTED;
    }

    tele_indicator_event_record_t *record = &s_events[s_event_count];
    memset(record, 0, sizeof(*record));
    if (!copy_text(record->id, sizeof(record->id), event->id) ||
        !copy_text(record->output_id, sizeof(record->output_id), event->output_id)) {
        unlock_indicator();
        return ESP_ERR_INVALID_ARG;
    }
    record->priority = event->priority;
    record->duration_ms = event->duration_ms;
    record->effect = event->effect;
    s_event_count++;
    unlock_indicator();
    return ESP_OK;
}

esp_err_t tele_indicator_raise(const char *source_id, const char *event_id)
{
    tele_signal_effect_t effect = {0};
    tele_indicator_output_apply_cb_t apply = NULL;
    void *ctx = NULL;
    bool should_apply = false;

    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    lock_indicator();
    int source_index = find_source(source_id);
    int event_index = find_event(event_id);
    if (source_index < 0 || event_index < 0) {
        unlock_indicator();
        return ESP_ERR_NOT_FOUND;
    }

    tele_indicator_active_slot_t *slot = &s_active[source_index];
#ifndef TELE_INDICATOR_HOST_TEST
    if (!slot->clear_timer) {
        esp_timer_create_args_t timer_args = {
            .callback = clear_timer_cb,
            .arg = (void *)(size_t)source_index,
            .name = "tele_indicator_clear",
        };
        esp_err_t err = esp_timer_create(&timer_args, &slot->clear_timer);
        if (err != ESP_OK) {
            unlock_indicator();
            return err;
        }
    } else {
        esp_timer_stop(slot->clear_timer);
    }
#endif

    const tele_indicator_event_record_t *event = &s_events[event_index];
#ifndef TELE_INDICATOR_HOST_TEST
    esp_timer_handle_t timer = slot->clear_timer;
#endif
    memset(slot, 0, sizeof(*slot));
#ifndef TELE_INDICATOR_HOST_TEST
    slot->clear_timer = timer;
#endif
    slot->active = true;
    copy_text(slot->source_id, sizeof(slot->source_id), source_id);
    copy_text(slot->event_id, sizeof(slot->event_id), event->id);
    copy_text(slot->output_id, sizeof(slot->output_id), event->output_id);
    slot->priority = event->priority;
    slot->sequence = ++s_sequence;
    slot->duration_ms = event->duration_ms;
    slot->effect = event->effect;
    if (event->duration_ms > 0) {
        slot->expires_at_ms = now_ms() + event->duration_ms;
    }
#ifndef TELE_INDICATOR_HOST_TEST
    if (event->duration_ms > 0) {
        esp_err_t err = esp_timer_start_once(slot->clear_timer, (uint64_t)event->duration_ms * 1000ULL);
        if (err != ESP_OK) {
            clear_slot_locked((size_t)source_index);
            unlock_indicator();
            return err;
        }
    }
#endif

    should_apply = prepare_apply_locked(&effect, &apply, &ctx);
    unlock_indicator();
    return apply_after_unlock(&effect, apply, ctx, should_apply);
}

esp_err_t tele_indicator_clear_source(const char *source_id)
{
    tele_signal_effect_t effect = {0};
    tele_indicator_output_apply_cb_t apply = NULL;
    void *ctx = NULL;
    bool should_apply = false;

    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    lock_indicator();
    int source_index = find_source(source_id);
    if (source_index < 0) {
        unlock_indicator();
        return ESP_ERR_NOT_FOUND;
    }
    clear_slot_locked((size_t)source_index);
    should_apply = prepare_apply_locked(&effect, &apply, &ctx);
    unlock_indicator();
    return apply_after_unlock(&effect, apply, ctx, should_apply);
}

esp_err_t tele_indicator_clear_event(const char *event_id)
{
    tele_signal_effect_t effect = {0};
    tele_indicator_output_apply_cb_t apply = NULL;
    void *ctx = NULL;
    bool should_apply = false;
    bool found = false;

    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    lock_indicator();
    for (size_t i = 0; i < s_source_count; ++i) {
        if (s_active[i].active && strcmp(s_active[i].event_id, event_id) == 0) {
            clear_slot_locked(i);
            found = true;
        }
    }
    if (!found) {
        unlock_indicator();
        return ESP_ERR_NOT_FOUND;
    }
    should_apply = prepare_apply_locked(&effect, &apply, &ctx);
    unlock_indicator();
    return apply_after_unlock(&effect, apply, ctx, should_apply);
}

esp_err_t tele_indicator_get_effective(tele_indicator_effective_t *out_effective)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!out_effective) {
        return ESP_ERR_INVALID_ARG;
    }

    lock_indicator();
    *out_effective = s_effective;
    unlock_indicator();
    return ESP_OK;
}

#ifdef TELE_INDICATOR_HOST_TEST
void tele_indicator_host_advance_time(uint32_t elapsed_ms)
{
    tele_signal_effect_t effect = {0};
    tele_indicator_output_apply_cb_t apply = NULL;
    void *ctx = NULL;
    bool should_apply = false;

    s_now_ms += elapsed_ms;

    for (size_t i = 0; i < s_source_count; ++i) {
        if (s_active[i].active &&
            s_active[i].duration_ms > 0 &&
            s_active[i].expires_at_ms <= s_now_ms) {
            clear_slot_locked(i);
        }
    }

    should_apply = prepare_apply_locked(&effect, &apply, &ctx);
    (void)apply_after_unlock(&effect, apply, ctx, should_apply);
}
#endif
