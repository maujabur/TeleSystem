#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#include "boot_config_button.h"

#ifndef CONFIG_BOOT_WIFI_CONFIG_BUTTON_GPIO
#define CONFIG_BOOT_WIFI_CONFIG_BUTTON_GPIO 3
#endif

#ifndef CONFIG_BOOT_WIFI_CONFIG_BUTTON_ACTIVE_LEVEL
#define CONFIG_BOOT_WIFI_CONFIG_BUTTON_ACTIVE_LEVEL 0
#endif

#ifndef CONFIG_BOOT_WIFI_CONFIG_BUTTON_SETTLE_MS
#define CONFIG_BOOT_WIFI_CONFIG_BUTTON_SETTLE_MS 20
#endif

#ifndef CONFIG_BOOT_WIFI_CONFIG_BUTTON_SAMPLE_COUNT
#define CONFIG_BOOT_WIFI_CONFIG_BUTTON_SAMPLE_COUNT 5
#endif

#ifndef CONFIG_BOOT_WIFI_CONFIG_BUTTON_SAMPLE_INTERVAL_MS
#define CONFIG_BOOT_WIFI_CONFIG_BUTTON_SAMPLE_INTERVAL_MS 10
#endif

static const char *TAG = "boot-button";

static void delay_ms(uint32_t delay)
{
    esp_rom_delay_us(delay * 1000U);
}

bool boot_config_button_is_pressed(void)
{
    const gpio_num_t gpio_num = (gpio_num_t)CONFIG_BOOT_WIFI_CONFIG_BUTTON_GPIO;
    const int active_level = CONFIG_BOOT_WIFI_CONFIG_BUTTON_ACTIVE_LEVEL ? 1 : 0;
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << CONFIG_BOOT_WIFI_CONFIG_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = active_level == 0 ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = active_level == 1 ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Nao foi possivel configurar IO%d: %s",
                 CONFIG_BOOT_WIFI_CONFIG_BUTTON_GPIO,
                 esp_err_to_name(err));
        return false;
    }

    delay_ms(CONFIG_BOOT_WIFI_CONFIG_BUTTON_SETTLE_MS);

    int raw_level = gpio_get_level(gpio_num);
    ESP_LOGI(TAG,
             "Boot button check: io=%d active_level=%d raw_level=%d pull=%s",
             CONFIG_BOOT_WIFI_CONFIG_BUTTON_GPIO,
             active_level,
             raw_level,
             active_level == 0 ? "pull-up" : "pull-down");

    for (int i = 0; i < CONFIG_BOOT_WIFI_CONFIG_BUTTON_SAMPLE_COUNT; ++i) {
        int sample_level = gpio_get_level(gpio_num);
        if (sample_level != active_level) {
            ESP_LOGI(TAG,
                     "Boot button not pressed: io=%d sample=%d/%d level=%d expected=%d",
                     CONFIG_BOOT_WIFI_CONFIG_BUTTON_GPIO,
                     i + 1,
                     CONFIG_BOOT_WIFI_CONFIG_BUTTON_SAMPLE_COUNT,
                     sample_level,
                     active_level);
            return false;
        }
        delay_ms(CONFIG_BOOT_WIFI_CONFIG_BUTTON_SAMPLE_INTERVAL_MS);
    }

    ESP_LOGW(TAG, "Botao de configuracao pressionado no boot (IO%d)",
             CONFIG_BOOT_WIFI_CONFIG_BUTTON_GPIO);
    return true;
}
