#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/cdefs.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_err.h"
#include "esp_log.h"
#include "soc/soc_caps.h"

#include "status_led.h"

#ifndef CONFIG_STATUS_LED_ENABLED
#define CONFIG_STATUS_LED_ENABLED 1
#endif

#ifndef CONFIG_STATUS_LED_GPIO
#define CONFIG_STATUS_LED_GPIO 48
#endif

#ifndef CONFIG_STATUS_LED_COUNT
#define CONFIG_STATUS_LED_COUNT 1
#endif

#ifndef CONFIG_STATUS_LED_BRIGHTNESS
#define CONFIG_STATUS_LED_BRIGHTNESS 48
#endif

#ifndef CONFIG_STATUS_LED_RMT_RESOLUTION_HZ
#define CONFIG_STATUS_LED_RMT_RESOLUTION_HZ 10000000
#endif

#ifndef CONFIG_STATUS_LED_TASK_STACK_SIZE
#define CONFIG_STATUS_LED_TASK_STACK_SIZE 3072
#endif

#ifndef CONFIG_STATUS_LED_TASK_PRIORITY
#define CONFIG_STATUS_LED_TASK_PRIORITY 3
#endif

#ifndef CONFIG_STATUS_LED_BOOT_COLOR
#define CONFIG_STATUS_LED_BOOT_COLOR 0x202020
#endif

#ifndef CONFIG_STATUS_LED_WIFI_CONNECTING_COLOR
#define CONFIG_STATUS_LED_WIFI_CONNECTING_COLOR 0x0040FF
#endif

#ifndef CONFIG_STATUS_LED_WIFI_PROVISIONING_COLOR
#define CONFIG_STATUS_LED_WIFI_PROVISIONING_COLOR 0xFF9000
#endif

#ifndef CONFIG_STATUS_LED_WIFI_CONNECTED_COLOR
#define CONFIG_STATUS_LED_WIFI_CONNECTED_COLOR 0x00B050
#endif

#ifndef CONFIG_STATUS_LED_ERROR_COLOR
#define CONFIG_STATUS_LED_ERROR_COLOR 0xFF0000
#endif

#ifndef CONFIG_STATUS_LED_ACR_UPLOADING_COLOR
#define CONFIG_STATUS_LED_ACR_UPLOADING_COLOR 0x8000FF
#endif

#ifndef CONFIG_STATUS_LED_ACR_WAITING_RESULT_COLOR
#define CONFIG_STATUS_LED_ACR_WAITING_RESULT_COLOR 0x00C8FF
#endif

#ifndef CONFIG_STATUS_LED_ACR_RESULT_HUMAN_COLOR
#define CONFIG_STATUS_LED_ACR_RESULT_HUMAN_COLOR 0x00FF00
#endif

#ifndef CONFIG_STATUS_LED_ACR_RESULT_AI_COLOR
#define CONFIG_STATUS_LED_ACR_RESULT_AI_COLOR 0xFF00A0
#endif

#ifndef CONFIG_STATUS_LED_BT_NEXT_ACTIVE_COLOR
#define CONFIG_STATUS_LED_BT_NEXT_ACTIVE_COLOR 0xFFFFFF
#endif

#ifndef CONFIG_STATUS_LED_BOOT_ON_MS
#define CONFIG_STATUS_LED_BOOT_ON_MS 100
#endif

#ifndef CONFIG_STATUS_LED_BOOT_OFF_MS
#define CONFIG_STATUS_LED_BOOT_OFF_MS 900
#endif

#ifndef CONFIG_STATUS_LED_WIFI_CONNECTING_ON_MS
#define CONFIG_STATUS_LED_WIFI_CONNECTING_ON_MS 150
#endif

#ifndef CONFIG_STATUS_LED_WIFI_CONNECTING_OFF_MS
#define CONFIG_STATUS_LED_WIFI_CONNECTING_OFF_MS 150
#endif

#ifndef CONFIG_STATUS_LED_WIFI_PROVISIONING_ON_MS
#define CONFIG_STATUS_LED_WIFI_PROVISIONING_ON_MS 700
#endif

#ifndef CONFIG_STATUS_LED_WIFI_PROVISIONING_OFF_MS
#define CONFIG_STATUS_LED_WIFI_PROVISIONING_OFF_MS 700
#endif

#ifndef CONFIG_STATUS_LED_ERROR_ON_MS
#define CONFIG_STATUS_LED_ERROR_ON_MS 100
#endif

#ifndef CONFIG_STATUS_LED_ERROR_OFF_MS
#define CONFIG_STATUS_LED_ERROR_OFF_MS 100
#endif

#ifndef CONFIG_STATUS_LED_ACR_UPLOADING_ON_MS
#define CONFIG_STATUS_LED_ACR_UPLOADING_ON_MS 120
#endif

#ifndef CONFIG_STATUS_LED_ACR_UPLOADING_OFF_MS
#define CONFIG_STATUS_LED_ACR_UPLOADING_OFF_MS 120
#endif

#ifndef CONFIG_STATUS_LED_ACR_WAITING_RESULT_ON_MS
#define CONFIG_STATUS_LED_ACR_WAITING_RESULT_ON_MS 250
#endif

#ifndef CONFIG_STATUS_LED_ACR_WAITING_RESULT_OFF_MS
#define CONFIG_STATUS_LED_ACR_WAITING_RESULT_OFF_MS 750
#endif

#ifndef CONFIG_STATUS_LED_CAPTURE_OVERLAY_COLOR
#define CONFIG_STATUS_LED_CAPTURE_OVERLAY_COLOR 0xFFFFFF
#endif

#ifndef CONFIG_STATUS_LED_CAPTURE_OVERLAY_ON_MS
#define CONFIG_STATUS_LED_CAPTURE_OVERLAY_ON_MS 40
#endif

#ifndef CONFIG_STATUS_LED_CAPTURE_OVERLAY_OFF_MS
#define CONFIG_STATUS_LED_CAPTURE_OVERLAY_OFF_MS 760
#endif

#ifndef CONFIG_STATUS_LED_LOW_BATTERY_COLOR
#define CONFIG_STATUS_LED_LOW_BATTERY_COLOR 0xFF2000
#endif

#ifndef CONFIG_STATUS_LED_LOW_BATTERY_ON_MS
#define CONFIG_STATUS_LED_LOW_BATTERY_ON_MS 200
#endif

#ifndef CONFIG_STATUS_LED_LOW_BATTERY_OFF_MS
#define CONFIG_STATUS_LED_LOW_BATTERY_OFF_MS 800
#endif

#define STATUS_LED_RMT_MEM_SYMBOLS SOC_RMT_MEM_WORDS_PER_CHANNEL
#define STATUS_LED_TRANSMIT_TIMEOUT_MS 100

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} status_led_ws28xx_encoder_t;

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} status_led_color_t;

typedef struct {
    status_led_color_t color;
    uint32_t on_ms;
    uint32_t off_ms;
    bool solid;
} status_led_pattern_t;

static const char *TAG = "status-led";

static rmt_channel_handle_t s_tx_channel;
static rmt_encoder_handle_t s_led_encoder;
static uint8_t s_led_pixels[CONFIG_STATUS_LED_COUNT * 3];
static volatile status_led_state_t s_state = STATUS_LED_STATE_BOOT;
static volatile bool s_capture_overlay;
static TaskHandle_t s_task_handle;
static bool s_started;

static status_led_color_t color_from_rgb(uint32_t rgb)
{
    return (status_led_color_t) {
        .red = (uint8_t)((rgb >> 16) & 0xFF),
        .green = (uint8_t)((rgb >> 8) & 0xFF),
        .blue = (uint8_t)(rgb & 0xFF),
    };
}

static uint8_t apply_brightness(uint8_t value)
{
    return (uint8_t)(((uint32_t)value * CONFIG_STATUS_LED_BRIGHTNESS) / 255U);
}

static uint32_t ns_to_rmt_ticks(uint32_t ns)
{
    uint64_t ticks = ((uint64_t)ns * CONFIG_STATUS_LED_RMT_RESOLUTION_HZ + 999999999ULL) / 1000000000ULL;
    return ticks > 0 ? (uint32_t)ticks : 1;
}

static status_led_pattern_t pattern_for_state(status_led_state_t state)
{
    switch (state) {
    case STATUS_LED_STATE_WIFI_CONNECTING:
        return (status_led_pattern_t) {
            .color = color_from_rgb(CONFIG_STATUS_LED_WIFI_CONNECTING_COLOR),
            .on_ms = CONFIG_STATUS_LED_WIFI_CONNECTING_ON_MS,
            .off_ms = CONFIG_STATUS_LED_WIFI_CONNECTING_OFF_MS,
        };
    case STATUS_LED_STATE_WIFI_PROVISIONING:
        return (status_led_pattern_t) {
            .color = color_from_rgb(CONFIG_STATUS_LED_WIFI_PROVISIONING_COLOR),
            .on_ms = CONFIG_STATUS_LED_WIFI_PROVISIONING_ON_MS,
            .off_ms = CONFIG_STATUS_LED_WIFI_PROVISIONING_OFF_MS,
        };
    case STATUS_LED_STATE_WIFI_CONNECTED:
        return (status_led_pattern_t) {
            .color = color_from_rgb(CONFIG_STATUS_LED_WIFI_CONNECTED_COLOR),
            .solid = true,
        };
    case STATUS_LED_STATE_ACR_UPLOADING:
        return (status_led_pattern_t) {
            .color = color_from_rgb(CONFIG_STATUS_LED_ACR_UPLOADING_COLOR),
            .on_ms = CONFIG_STATUS_LED_ACR_UPLOADING_ON_MS,
            .off_ms = CONFIG_STATUS_LED_ACR_UPLOADING_OFF_MS,
        };
    case STATUS_LED_STATE_ACR_WAITING_RESULT:
        return (status_led_pattern_t) {
            .color = color_from_rgb(CONFIG_STATUS_LED_ACR_WAITING_RESULT_COLOR),
            .on_ms = CONFIG_STATUS_LED_ACR_WAITING_RESULT_ON_MS,
            .off_ms = CONFIG_STATUS_LED_ACR_WAITING_RESULT_OFF_MS,
        };
    case STATUS_LED_STATE_ACR_RESULT_HUMAN:
        return (status_led_pattern_t) {
            .color = color_from_rgb(CONFIG_STATUS_LED_ACR_RESULT_HUMAN_COLOR),
            .solid = true,
        };
    case STATUS_LED_STATE_ACR_RESULT_AI:
        return (status_led_pattern_t) {
            .color = color_from_rgb(CONFIG_STATUS_LED_ACR_RESULT_AI_COLOR),
            .solid = true,
        };
    case STATUS_LED_STATE_BT_NEXT_ACTIVE:
        return (status_led_pattern_t) {
            .color = color_from_rgb(CONFIG_STATUS_LED_BT_NEXT_ACTIVE_COLOR),
            .solid = true,
        };
    case STATUS_LED_STATE_ERROR:
        return (status_led_pattern_t) {
            .color = color_from_rgb(CONFIG_STATUS_LED_ERROR_COLOR),
            .on_ms = CONFIG_STATUS_LED_ERROR_ON_MS,
            .off_ms = CONFIG_STATUS_LED_ERROR_OFF_MS,
        };
    case STATUS_LED_STATE_LOW_BATTERY:
        return (status_led_pattern_t) {
            .color = color_from_rgb(CONFIG_STATUS_LED_LOW_BATTERY_COLOR),
            .on_ms = CONFIG_STATUS_LED_LOW_BATTERY_ON_MS,
            .off_ms = CONFIG_STATUS_LED_LOW_BATTERY_OFF_MS,
        };
    case STATUS_LED_STATE_BOOT:
    default:
        return (status_led_pattern_t) {
            .color = color_from_rgb(CONFIG_STATUS_LED_BOOT_COLOR),
            .on_ms = CONFIG_STATUS_LED_BOOT_ON_MS,
            .off_ms = CONFIG_STATUS_LED_BOOT_OFF_MS,
        };
    }
}

static void set_pixel_buffer(status_led_color_t color)
{
    uint8_t channels[3] = {
        apply_brightness(color.red),
        apply_brightness(color.green),
        apply_brightness(color.blue),
    };
#if CONFIG_STATUS_LED_COLOR_ORDER_RGB
    uint8_t wire_order[3] = {0, 1, 2};
#elif CONFIG_STATUS_LED_COLOR_ORDER_RBG
    uint8_t wire_order[3] = {0, 2, 1};
#elif CONFIG_STATUS_LED_COLOR_ORDER_GRB
    uint8_t wire_order[3] = {1, 0, 2};
#elif CONFIG_STATUS_LED_COLOR_ORDER_GBR
    uint8_t wire_order[3] = {1, 2, 0};
#elif CONFIG_STATUS_LED_COLOR_ORDER_BRG
    uint8_t wire_order[3] = {2, 0, 1};
#elif CONFIG_STATUS_LED_COLOR_ORDER_BGR
    uint8_t wire_order[3] = {2, 1, 0};
#else
    uint8_t wire_order[3] = {1, 0, 2};
#endif

    for (size_t i = 0; i < CONFIG_STATUS_LED_COUNT; ++i) {
        s_led_pixels[(i * 3) + 0] = channels[wire_order[0]];
        s_led_pixels[(i * 3) + 1] = channels[wire_order[1]];
        s_led_pixels[(i * 3) + 2] = channels[wire_order[2]];
    }
}

static esp_err_t show_color(status_led_color_t color)
{
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    esp_err_t err = ESP_OK;

    set_pixel_buffer(color);
    err = rmt_transmit(s_tx_channel,
                       s_led_encoder,
                       s_led_pixels,
                       sizeof(s_led_pixels),
                       &tx_config);
    if (err != ESP_OK) {
        return err;
    }

    return rmt_tx_wait_all_done(s_tx_channel, STATUS_LED_TRANSMIT_TIMEOUT_MS);
}

static esp_err_t clear_led(void)
{
    return show_color((status_led_color_t) {0});
}

RMT_ENCODER_FUNC_ATTR
static size_t status_led_encode_ws28xx(rmt_encoder_t *encoder,
                                       rmt_channel_handle_t channel,
                                       const void *primary_data,
                                       size_t data_size,
                                       rmt_encode_state_t *ret_state)
{
    status_led_ws28xx_encoder_t *led_encoder = __containerof(encoder,
                                                             status_led_ws28xx_encoder_t,
                                                             base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;

    switch (led_encoder->state) {
    case 0:
        encoded_symbols += led_encoder->bytes_encoder->encode(led_encoder->bytes_encoder,
                                                              channel,
                                                              primary_data,
                                                              data_size,
                                                              &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            led_encoder->state = 1;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
        __attribute__((fallthrough));
    case 1:
        encoded_symbols += led_encoder->copy_encoder->encode(led_encoder->copy_encoder,
                                                             channel,
                                                             &led_encoder->reset_code,
                                                             sizeof(led_encoder->reset_code),
                                                             &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            state |= RMT_ENCODING_COMPLETE;
            led_encoder->state = RMT_ENCODING_RESET;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
        }
        break;
    default:
        led_encoder->state = RMT_ENCODING_RESET;
        break;
    }

out:
    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t status_led_del_ws28xx_encoder(rmt_encoder_t *encoder)
{
    status_led_ws28xx_encoder_t *led_encoder = __containerof(encoder,
                                                             status_led_ws28xx_encoder_t,
                                                             base);
    rmt_del_encoder(led_encoder->bytes_encoder);
    rmt_del_encoder(led_encoder->copy_encoder);
    free(led_encoder);
    return ESP_OK;
}

RMT_ENCODER_FUNC_ATTR
static esp_err_t status_led_ws28xx_encoder_reset(rmt_encoder_t *encoder)
{
    status_led_ws28xx_encoder_t *led_encoder = __containerof(encoder,
                                                             status_led_ws28xx_encoder_t,
                                                             base);
    rmt_encoder_reset(led_encoder->bytes_encoder);
    rmt_encoder_reset(led_encoder->copy_encoder);
    led_encoder->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

static esp_err_t status_led_new_ws28xx_encoder(rmt_encoder_handle_t *ret_encoder)
{
    status_led_ws28xx_encoder_t *led_encoder = NULL;
    rmt_bytes_encoder_config_t bytes_encoder_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = ns_to_rmt_ticks(300),
            .level1 = 0,
            .duration1 = ns_to_rmt_ticks(900),
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = ns_to_rmt_ticks(900),
            .level1 = 0,
            .duration1 = ns_to_rmt_ticks(300),
        },
        .flags.msb_first = 1,
    };
    rmt_copy_encoder_config_t copy_encoder_config = {};
    esp_err_t err = ESP_OK;

    if (!ret_encoder) {
        return ESP_ERR_INVALID_ARG;
    }

    led_encoder = rmt_alloc_encoder_mem(sizeof(status_led_ws28xx_encoder_t));
    if (!led_encoder) {
        return ESP_ERR_NO_MEM;
    }

    led_encoder->base.encode = status_led_encode_ws28xx;
    led_encoder->base.del = status_led_del_ws28xx_encoder;
    led_encoder->base.reset = status_led_ws28xx_encoder_reset;
    led_encoder->reset_code = (rmt_symbol_word_t) {
        .level0 = 0,
        .duration0 = ns_to_rmt_ticks(25000),
        .level1 = 0,
        .duration1 = ns_to_rmt_ticks(25000),
    };

    err = rmt_new_bytes_encoder(&bytes_encoder_config, &led_encoder->bytes_encoder);
    if (err != ESP_OK) {
        free(led_encoder);
        return err;
    }

    err = rmt_new_copy_encoder(&copy_encoder_config, &led_encoder->copy_encoder);
    if (err != ESP_OK) {
        rmt_del_encoder(led_encoder->bytes_encoder);
        free(led_encoder);
        return err;
    }

    *ret_encoder = &led_encoder->base;
    return ESP_OK;
}

static void status_led_task(void *arg)
{
    (void)arg;

    status_led_state_t last_state = (status_led_state_t)(STATUS_LED_STATE_ERROR + 1);
    uint32_t state_phase_start_ms = 0;
    status_led_color_t last_applied_color = {0};
    bool has_last_applied_color = false;

    while (true) {
        status_led_state_t state = s_state;
        status_led_pattern_t pattern = pattern_for_state(state);
        uint32_t now_ms = esp_log_timestamp();
        bool base_on = true;
        status_led_color_t target_color = {0};

        if (state != last_state) {
            last_state = state;
            state_phase_start_ms = now_ms;
        }

        if (!pattern.solid) {
            uint32_t cycle_ms = pattern.on_ms + pattern.off_ms;
            if (cycle_ms == 0 || pattern.on_ms == 0) {
                base_on = false;
            } else {
                uint32_t phase_ms = (now_ms - state_phase_start_ms) % cycle_ms;
                base_on = phase_ms < pattern.on_ms;
            }
        }

        if (base_on) {
            target_color = pattern.color;
        }

        if (s_capture_overlay) {
            const uint32_t pulse_period_ms =
                CONFIG_STATUS_LED_CAPTURE_OVERLAY_ON_MS + CONFIG_STATUS_LED_CAPTURE_OVERLAY_OFF_MS;
            if (pulse_period_ms > 0 && CONFIG_STATUS_LED_CAPTURE_OVERLAY_ON_MS > 0) {
                uint32_t pulse_phase_ms = now_ms % pulse_period_ms;
                if (pulse_phase_ms < CONFIG_STATUS_LED_CAPTURE_OVERLAY_ON_MS) {
                    target_color = color_from_rgb(CONFIG_STATUS_LED_CAPTURE_OVERLAY_COLOR);
                }
            }
        }

        if (!has_last_applied_color ||
            memcmp(&target_color, &last_applied_color, sizeof(target_color)) != 0) {
            if (target_color.red == 0 && target_color.green == 0 && target_color.blue == 0) {
                clear_led();
            } else {
                show_color(target_color);
            }
            last_applied_color = target_color;
            has_last_applied_color = true;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t status_led_start(void)
{
#if CONFIG_STATUS_LED_ENABLED
    rmt_tx_channel_config_t tx_channel_config = {
        .gpio_num = CONFIG_STATUS_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = CONFIG_STATUS_LED_RMT_RESOLUTION_HZ,
        .mem_block_symbols = STATUS_LED_RMT_MEM_SYMBOLS,
        .trans_queue_depth = 2,
    };
    esp_err_t err = ESP_OK;

    if (s_started) {
        return ESP_OK;
    }

    err = rmt_new_tx_channel(&tx_channel_config, &s_tx_channel);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Nao foi possivel criar canal RMT do LED: %s", esp_err_to_name(err));
        return err;
    }

    err = status_led_new_ws28xx_encoder(&s_led_encoder);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Nao foi possivel criar encoder WS28xx: %s", esp_err_to_name(err));
        return err;
    }

    err = rmt_enable(s_tx_channel);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Nao foi possivel habilitar canal RMT do LED: %s", esp_err_to_name(err));
        return err;
    }

    s_state = STATUS_LED_STATE_BOOT;
    if (xTaskCreate(status_led_task,
                    "status_led",
                    CONFIG_STATUS_LED_TASK_STACK_SIZE,
                    NULL,
                    CONFIG_STATUS_LED_TASK_PRIORITY,
                    &s_task_handle) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    return ESP_OK;
#else
    return ESP_OK;
#endif
}

status_led_state_t status_led_get_state(void)
{
    return s_state;
}

esp_err_t status_led_set_state(status_led_state_t state)
{
#if CONFIG_STATUS_LED_ENABLED
    if (state > STATUS_LED_STATE_ERROR) {
        return ESP_ERR_INVALID_ARG;
    }

    s_state = state;
    return ESP_OK;
#else
    (void)state;
    return ESP_OK;
#endif
}

esp_err_t status_led_set_capture_overlay(bool enabled)
{
#if CONFIG_STATUS_LED_ENABLED
    s_capture_overlay = enabled;
    return ESP_OK;
#else
    (void)enabled;
    return ESP_OK;
#endif
}
