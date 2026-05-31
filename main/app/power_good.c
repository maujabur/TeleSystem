#include "power_good.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "power-good";

#ifndef CONFIG_POWER_GOOD_GPIO_ENABLED
#define CONFIG_POWER_GOOD_GPIO_ENABLED 0
#endif

#ifndef CONFIG_POWER_GOOD_GPIO
#define CONFIG_POWER_GOOD_GPIO 6
#endif

#ifndef CONFIG_POWER_GOOD_ACTIVE_LEVEL
#define CONFIG_POWER_GOOD_ACTIVE_LEVEL 1
#endif

static bool s_initialized;

static int inactive_level(void)
{
    return CONFIG_POWER_GOOD_ACTIVE_LEVEL ? 0 : 1;
}

static int active_level(void)
{
    return CONFIG_POWER_GOOD_ACTIVE_LEVEL ? 1 : 0;
}

esp_err_t power_good_init(void)
{
    if (!CONFIG_POWER_GOOD_GPIO_ENABLED) {
        ESP_LOGI(TAG, "POWER_GOOD desabilitado por configuracao");
        s_initialized = false;
        return ESP_OK;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << CONFIG_POWER_GOOD_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Falha ao configurar GPIO %d: %s",
                 CONFIG_POWER_GOOD_GPIO,
                 esp_err_to_name(err));
        return err;
    }

    /* Garante que perifericos comecam desligados ate autorizacao explicita */
    gpio_set_level((gpio_num_t)CONFIG_POWER_GOOD_GPIO, inactive_level());
    s_initialized = true;

    ESP_LOGI(TAG,
             "GPIO %d configurada | active_level=%d | estado=off",
             CONFIG_POWER_GOOD_GPIO,
             CONFIG_POWER_GOOD_ACTIVE_LEVEL);
    return ESP_OK;
}

void power_good_set(bool on)
{
    if (!s_initialized) {
        return;
    }

    int level = on ? active_level() : inactive_level();
    gpio_set_level((gpio_num_t)CONFIG_POWER_GOOD_GPIO, level);
    ESP_LOGI(TAG, "POWER_GOOD=%s (gpio=%d level=%d)", on ? "on" : "off", CONFIG_POWER_GOOD_GPIO, level);
}
