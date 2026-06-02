#include "esp_log.h"
#include "esp_psram.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "acr_analysis_control.h"
#include "acr_orchestrator.h"
#include "acr_routes.h"
#include "acr_trigger_output.h"
#include "app_log_buffer.h"
#include "connectivity_controller.h"
#include "device_config_routes.h"
#include "firmware_ota.h"
#include "firmware_version.h"
#include "mqtt_presence.h"
#include "ota_portal.h"
#include "power_good.h"
#include "vbat_monitor.h"
#include "wm8782_probe.h"

#ifndef CONFIG_VBAT_SHUTDOWN_THRESHOLD_MV
#define CONFIG_VBAT_SHUTDOWN_THRESHOLD_MV 3500
#endif

#ifndef CONFIG_VBAT_MONITOR_ENABLED
#define CONFIG_VBAT_MONITOR_ENABLED 0
#endif

#ifndef CONFIG_VBAT_MONITOR_GPIO
#define CONFIG_VBAT_MONITOR_GPIO 1
#endif

#ifndef CONFIG_VBAT_MONITOR_MAX_INTERVAL_S
#define CONFIG_VBAT_MONITOR_MAX_INTERVAL_S 120
#endif

#ifndef CONFIG_VBAT_SHUTDOWN_ENABLED
#define CONFIG_VBAT_SHUTDOWN_ENABLED 0
#endif

#ifndef CONFIG_VBAT_SHUTDOWN_DEBOUNCE_MS
#define CONFIG_VBAT_SHUTDOWN_DEBOUNCE_MS 1000
#endif

#ifndef CONFIG_POWER_GOOD_GPIO_ENABLED
#define CONFIG_POWER_GOOD_GPIO_ENABLED 0
#endif

#ifndef CONFIG_POWER_GOOD_GPIO
#define CONFIG_POWER_GOOD_GPIO 6
#endif

#ifndef CONFIG_POWER_GOOD_ACTIVE_LEVEL
#define CONFIG_POWER_GOOD_ACTIVE_LEVEL 1
#endif

#ifndef CONFIG_STATUS_LED_ENABLED
#define CONFIG_STATUS_LED_ENABLED 1
#endif

#ifndef CONFIG_STATUS_LED_GPIO
#define CONFIG_STATUS_LED_GPIO 48
#endif

#ifndef CONFIG_ACR_AUTO_ENABLED
#define CONFIG_ACR_AUTO_ENABLED 0
#endif

#ifndef CONFIG_ACR_AUTO_INTERVAL_MS
#define CONFIG_ACR_AUTO_INTERVAL_MS 60000
#endif

#ifndef CONFIG_ACR_CAPTURE_DURATION_SECONDS
#define CONFIG_ACR_CAPTURE_DURATION_SECONDS 8
#endif

static const char *TAG = "skips";

#ifndef CONFIG_ACR_ORCHESTRATOR_TASK_STACK_SIZE
#define CONFIG_ACR_ORCHESTRATOR_TASK_STACK_SIZE 12288
#endif

#ifndef CONFIG_ACR_ORCHESTRATOR_TASK_PRIORITY
#define CONFIG_ACR_ORCHESTRATOR_TASK_PRIORITY 5
#endif

static void acr_orchestrator_task(void *arg)
{
    (void)arg;
    acr_orchestrator_run();
    vTaskDelete(NULL);
}

static void log_startup_config_snapshot(void)
{
    ESP_LOGI(TAG,
             "Config snapshot | vbat_monitor=%s | vbat_gpio=%d | vbat_max_interval_s=%d",
             CONFIG_VBAT_MONITOR_ENABLED ? "on" : "off",
             CONFIG_VBAT_MONITOR_GPIO,
             CONFIG_VBAT_MONITOR_MAX_INTERVAL_S);
    ESP_LOGI(TAG,
             "Config snapshot | low_battery_shutdown=%s | threshold_mv=%d | debounce_ms=%d",
             CONFIG_VBAT_SHUTDOWN_ENABLED ? "on" : "off",
             CONFIG_VBAT_SHUTDOWN_THRESHOLD_MV,
             CONFIG_VBAT_SHUTDOWN_DEBOUNCE_MS);
    ESP_LOGI(TAG,
             "Config snapshot | power_good=%s | gpio=%d | active_level=%d",
             CONFIG_POWER_GOOD_GPIO_ENABLED ? "on" : "off",
             CONFIG_POWER_GOOD_GPIO,
             CONFIG_POWER_GOOD_ACTIVE_LEVEL);
    ESP_LOGI(TAG,
             "Config snapshot | status_led=%s | status_led_gpio=%d",
             CONFIG_STATUS_LED_ENABLED ? "on" : "off",
             CONFIG_STATUS_LED_GPIO);
    ESP_LOGI(TAG,
             "Config snapshot | acr_auto=%s | acr_interval_ms=%d | acr_capture_s=%d | orch_stack=%d | orch_prio=%d",
             CONFIG_ACR_AUTO_ENABLED ? "on" : "off",
             CONFIG_ACR_AUTO_INTERVAL_MS,
             CONFIG_ACR_CAPTURE_DURATION_SECONDS,
             CONFIG_ACR_ORCHESTRATOR_TASK_STACK_SIZE,
             CONFIG_ACR_ORCHESTRATOR_TASK_PRIORITY);
}

void app_main(void)
{
    esp_err_t err;

    app_log_buffer_init();
    ESP_ERROR_CHECK(vbat_monitor_init());

    ESP_LOGI(TAG, "Inicializando");
    log_startup_config_snapshot();
    ESP_LOGI(TAG, "Versao do firmware: %s", APP_VERSION_STRING);
    ESP_LOGI(TAG, "PSRAM detectada: %u bytes", (unsigned)esp_psram_get_size());
    vbat_monitor_maybe_measure(VBAT_MONITOR_MOMENT_BOOT);

    ESP_ERROR_CHECK(power_good_init());
    if (vbat_monitor_is_below_threshold()) {
        vbat_monitor_status_t vbat_status = {0};
        vbat_monitor_get_status(&vbat_status);
        ESP_LOGE(TAG,
                 "VBAT critica no boot: %dmV < %dmV — desligando perifericos e entrando em deep sleep",
                 vbat_status.vbat_mv,
                 CONFIG_VBAT_SHUTDOWN_THRESHOLD_MV);
        power_good_set(false);
        esp_deep_sleep_start();
    }
    power_good_set(true);
    wm8782_probe_result_t wm8782_result = {0};
    err = wm8782_probe(&wm8782_result);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "WM8782 %s | bytes=%u | min=%d | max=%d",
                 wm8782_result.present ? "detectado" : "nao detectado",
                 (unsigned)wm8782_result.bytes_read,
                 (int)wm8782_result.min_sample,
                 (int)wm8782_result.max_sample);
    } else {
        ESP_LOGE(TAG, "Falha no probe WM8782: %s", esp_err_to_name(err));
    }

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    acr_analysis_control_init();

    ESP_ERROR_CHECK(acr_trigger_output_init());
    ESP_ERROR_CHECK(acr_routes_register_with_portal());
    ESP_ERROR_CHECK(firmware_ota_init());
    ESP_ERROR_CHECK(ota_portal_register_with_portal());
    ESP_ERROR_CHECK(device_config_routes_register_with_portal());
    ESP_ERROR_CHECK(connectivity_controller_start());
    ESP_ERROR_CHECK(mqtt_presence_start());

    BaseType_t task_ok = xTaskCreate(acr_orchestrator_task,
                                     "acr_orch",
                                     CONFIG_ACR_ORCHESTRATOR_TASK_STACK_SIZE,
                                     NULL,
                                     CONFIG_ACR_ORCHESTRATOR_TASK_PRIORITY,
                                     NULL);
    ESP_ERROR_CHECK(task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
