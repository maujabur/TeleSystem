#include "wm8782_probe.h"

#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "wm8782";

#define WM8782_SAMPLE_RATE_HZ 16000
#define WM8782_PROBE_SAMPLES 256
#define WM8782_READ_TIMEOUT_MS 200
#define WM8782_MIN_SWING_FOR_PRESENT 2

#ifndef CONFIG_AUDIO_I2S_MCLK_GPIO
#define CONFIG_AUDIO_I2S_MCLK_GPIO 1
#endif

#ifndef CONFIG_AUDIO_I2S_DIN_GPIO
#define CONFIG_AUDIO_I2S_DIN_GPIO 2
#endif

#ifndef CONFIG_AUDIO_I2S_WS_GPIO
#define CONFIG_AUDIO_I2S_WS_GPIO 3
#endif

#ifndef CONFIG_AUDIO_I2S_BCLK_GPIO
#define CONFIG_AUDIO_I2S_BCLK_GPIO 4
#endif

esp_err_t wm8782_probe(wm8782_probe_result_t *result)
{
    ESP_RETURN_ON_FALSE(result != NULL, ESP_ERR_INVALID_ARG, TAG, "resultado nulo");
    memset(result, 0, sizeof(*result));

    const int mclk_cfg_gpio = CONFIG_AUDIO_I2S_MCLK_GPIO;
    const gpio_num_t mclk_gpio =
        mclk_cfg_gpio < 0 ? I2S_GPIO_UNUSED : (gpio_num_t)mclk_cfg_gpio;
    const gpio_num_t bclk_gpio = (gpio_num_t)CONFIG_AUDIO_I2S_BCLK_GPIO;
    const gpio_num_t ws_gpio = (gpio_num_t)CONFIG_AUDIO_I2S_WS_GPIO;
    const gpio_num_t din_gpio = (gpio_num_t)CONFIG_AUDIO_I2S_DIN_GPIO;

    i2s_chan_handle_t rx_handle = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 128;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
    ESP_RETURN_ON_ERROR(err, TAG, "falha ao criar canal I2S RX");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(WM8782_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = mclk_gpio,
            .bclk = bclk_gpio,
            .ws = ws_gpio,
            .dout = I2S_GPIO_UNUSED,
            .din = din_gpio,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_LOGI(TAG,
             "I2S GPIO cfg: mclk_cfg=%d (%s), bclk=%d, ws=%d, din=%d",
             mclk_cfg_gpio,
             mclk_gpio == I2S_GPIO_UNUSED ? "unused" : "enabled",
             (int)bclk_gpio,
             (int)ws_gpio,
             (int)din_gpio);

    err = i2s_channel_init_std_mode(rx_handle, &std_cfg);
    if (err != ESP_OK) {
        i2s_del_channel(rx_handle);
        ESP_LOGE(TAG, "falha ao configurar I2S STD: %s", esp_err_to_name(err));
        return err;
    }

    i2s_chan_info_t info = {0};
    err = i2s_channel_get_info(rx_handle, &info);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "I2S RX: mclk=%" PRIu32 " Hz, bclk=%" PRIu32 " Hz, sample_rate=%d Hz",
                 info.mclk_hz,
                 info.bclk_hz,
                 WM8782_SAMPLE_RATE_HZ);
    }

    int16_t samples[WM8782_PROBE_SAMPLES] = {0};
    size_t bytes_read = 0;

    err = i2s_channel_enable(rx_handle);
    if (err == ESP_OK) {
        err = i2s_channel_read(rx_handle,
                               samples,
                               sizeof(samples),
                               &bytes_read,
                               WM8782_READ_TIMEOUT_MS);
        esp_err_t disable_err = i2s_channel_disable(rx_handle);
        if (disable_err != ESP_OK) {
            ESP_LOGW(TAG, "falha ao parar I2S RX: %s", esp_err_to_name(disable_err));
        }
    }

    esp_err_t del_err = i2s_del_channel(rx_handle);
    if (del_err != ESP_OK) {
        ESP_LOGW(TAG, "falha ao liberar canal I2S RX: %s", esp_err_to_name(del_err));
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "falha ao ler amostras do WM8782: %s", esp_err_to_name(err));
        return err;
    }

    int16_t min_sample = INT16_MAX;
    int16_t max_sample = INT16_MIN;
    size_t sample_count = bytes_read / sizeof(samples[0]);

    for (size_t i = 0; i < sample_count; ++i) {
        if (samples[i] < min_sample) {
            min_sample = samples[i];
        }
        if (samples[i] > max_sample) {
            max_sample = samples[i];
        }
    }

    result->bytes_read = bytes_read;
    result->min_sample = sample_count > 0 ? min_sample : 0;
    result->max_sample = sample_count > 0 ? max_sample : 0;
    result->present = sample_count > 0 &&
                      ((int32_t)result->max_sample - (int32_t)result->min_sample) >=
                          WM8782_MIN_SWING_FOR_PRESENT;

    ESP_LOGI(TAG,
             "probe WM8782: %s, bytes=%u, min=%d, max=%d",
             result->present ? "sinal I2S detectado" : "sem variacao util",
             (unsigned)result->bytes_read,
             (int)result->min_sample,
             (int)result->max_sample);

    return ESP_OK;
}
