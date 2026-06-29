#include "app_indicators.h"

#include <stdbool.h>
#include <stddef.h>

#include "esp_check.h"
#include "status_led.h"
#include "tele_indicator.h"

static const char *TAG = "app-indicators";

static bool s_started;

static esp_err_t app_status_led_output_apply(const tele_signal_effect_t *effect, void *ctx)
{
    (void)ctx;
    return status_led_apply_effect(effect);
}

static const tele_indicator_output_t s_status_led_output = {
    .id = TELE_INDICATOR_DEFAULT_OUTPUT_ID,
    .apply = app_status_led_output_apply,
};

static const tele_indicator_source_t s_indicator_sources[] = {
    {.id = "system", .default_priority = 100},
    {.id = "wifi", .default_priority = 20},
    {.id = "product", .default_priority = 40},
    {.id = "battery", .default_priority = 120},
    {.id = "output", .default_priority = 70},
};

static const tele_indicator_event_t s_indicator_events[] = {
    {
        .id = "system.boot",
        .output_id = TELE_INDICATOR_DEFAULT_OUTPUT_ID,
        .priority = 10,
        .duration_ms = 0,
        .effect = {
            .id = TELE_SIGNAL_EFFECT_BREATH,
            .color_a = {.red = 0, .green = 0, .blue = 255},
            .time_a_ms = 1200,
            .brightness = 64,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
    {
        .id = "wifi.connecting",
        .output_id = TELE_INDICATOR_DEFAULT_OUTPUT_ID,
        .priority = 20,
        .duration_ms = 0,
        .effect = {
            .id = TELE_SIGNAL_EFFECT_BLINK,
            .color_a = {.red = 0, .green = 0, .blue = 255},
            .time_a_ms = 250,
            .time_b_ms = 750,
            .brightness = 80,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
    {
        .id = "wifi.provisioning",
        .output_id = TELE_INDICATOR_DEFAULT_OUTPUT_ID,
        .priority = 30,
        .duration_ms = 0,
        .effect = {
            .id = TELE_SIGNAL_EFFECT_ALTERNATE,
            .color_a = {.red = 0, .green = 0, .blue = 255},
            .color_b = {.red = 128, .green = 0, .blue = 255},
            .time_a_ms = 400,
            .time_b_ms = 400,
            .brightness = 90,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
    {
        .id = "wifi.connected",
        .output_id = TELE_INDICATOR_DEFAULT_OUTPUT_ID,
        .priority = 15,
        .duration_ms = 1500,
        .effect = {
            .id = TELE_SIGNAL_EFFECT_SOLID,
            .color_a = {.red = 0, .green = 255, .blue = 0},
            .brightness = 80,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
    {
        .id = "product.waiting",
        .output_id = TELE_INDICATOR_DEFAULT_OUTPUT_ID,
        .priority = 35,
        .duration_ms = 0,
        .effect = {
            .id = TELE_SIGNAL_EFFECT_BREATH,
            .color_a = {.red = 255, .green = 180, .blue = 0},
            .time_a_ms = 1400,
            .brightness = 70,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
    {
        .id = "product.transmitting",
        .output_id = TELE_INDICATOR_DEFAULT_OUTPUT_ID,
        .priority = 50,
        .duration_ms = 0,
        .effect = {
            .id = TELE_SIGNAL_EFFECT_BLINK,
            .color_a = {.red = 255, .green = 180, .blue = 0},
            .time_a_ms = 120,
            .time_b_ms = 120,
            .brightness = 100,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
    {
        .id = "product.result_ok",
        .output_id = TELE_INDICATOR_DEFAULT_OUTPUT_ID,
        .priority = 80,
        .duration_ms = 1200,
        .effect = {
            .id = TELE_SIGNAL_EFFECT_SOLID,
            .color_a = {.red = 0, .green = 255, .blue = 0},
            .brightness = 100,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
    {
        .id = "product.result_alert",
        .output_id = TELE_INDICATOR_DEFAULT_OUTPUT_ID,
        .priority = 90,
        .duration_ms = 2000,
        .effect = {
            .id = TELE_SIGNAL_EFFECT_BLINK,
            .color_a = {.red = 255, .green = 80, .blue = 0},
            .time_a_ms = 180,
            .time_b_ms = 180,
            .brightness = 120,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
    {
        .id = "output.active",
        .output_id = TELE_INDICATOR_DEFAULT_OUTPUT_ID,
        .priority = 70,
        .duration_ms = 0,
        .effect = {
            .id = TELE_SIGNAL_EFFECT_SOLID,
            .color_a = {.red = 255, .green = 255, .blue = 255},
            .brightness = 80,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
    {
        .id = "battery.low",
        .output_id = TELE_INDICATOR_DEFAULT_OUTPUT_ID,
        .priority = 180,
        .duration_ms = 0,
        .effect = {
            .id = TELE_SIGNAL_EFFECT_HEARTBEAT,
            .color_a = {.red = 255, .green = 120, .blue = 0},
            .time_a_ms = 1800,
            .brightness = 100,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
    {
        .id = "system.error",
        .output_id = TELE_INDICATOR_DEFAULT_OUTPUT_ID,
        .priority = 250,
        .duration_ms = 0,
        .effect = {
            .id = TELE_SIGNAL_EFFECT_BLINK,
            .color_a = {.red = 255, .green = 0, .blue = 0},
            .time_a_ms = 120,
            .time_b_ms = 120,
            .brightness = 120,
            .target_mask = TELE_SIGNAL_TARGET_ALL,
        },
    },
};

esp_err_t app_indicators_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(status_led_start(), TAG, "status_led_start failed");
    ESP_RETURN_ON_ERROR(tele_indicator_init(), TAG, "tele_indicator_init failed");

    tele_indicator_output_t status_led_output = s_status_led_output;
    status_led_output.supported_effect_ids = status_led_supported_effect_ids();
    status_led_output.supported_effect_count = status_led_supported_effect_count();

    ESP_RETURN_ON_ERROR(tele_indicator_register_output(&status_led_output),
                        TAG,
                        "register status_led output failed");

    for (size_t i = 0; i < sizeof(s_indicator_sources) / sizeof(s_indicator_sources[0]); ++i) {
        ESP_RETURN_ON_ERROR(tele_indicator_register_source(&s_indicator_sources[i]),
                            TAG,
                            "register source failed");
    }

    for (size_t i = 0; i < sizeof(s_indicator_events) / sizeof(s_indicator_events[0]); ++i) {
        ESP_RETURN_ON_ERROR(tele_indicator_register_event(&s_indicator_events[i]),
                            TAG,
                            "register event failed");
    }

    ESP_RETURN_ON_ERROR(tele_indicator_raise("system", "system.boot"),
                        TAG,
                        "raise boot indicator failed");
    s_started = true;
    return ESP_OK;
}
