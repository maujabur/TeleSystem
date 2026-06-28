#include "esp_check.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "connectivity_controller.h"
#include "firmware_ota.h"
#include "firmware_version.h"
#include "power_good.h"
#include "tele_artifacts.h"
#include "tele_ca_store.h"
#include "tele_ca_updater.h"
#include "tele_presence.h"
#include "tele_portal_commands.h"
#include "tele_portal_core.h"
#include "tele_portal_logs.h"
#include "vbat_monitor.h"
#include "wifi_manager.h"

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

#ifndef CONFIG_TELE_CA_UPDATER_BOOT_ENABLED
#define CONFIG_TELE_CA_UPDATER_BOOT_ENABLED 0
#endif

#ifndef CONFIG_TELE_CA_UPDATER_MANIFEST_URL
#define CONFIG_TELE_CA_UPDATER_MANIFEST_URL ""
#endif

#ifndef CONFIG_TELE_CA_UPDATER_CHANNEL
#define CONFIG_TELE_CA_UPDATER_CHANNEL "stable"
#endif

#ifndef CONFIG_TELE_CA_UPDATER_RESTART_ON_UPDATE
#define CONFIG_TELE_CA_UPDATER_RESTART_ON_UPDATE 0
#endif

#ifndef CONFIG_TELE_CA_UPDATER_BOOT_WAIT_TIMEOUT_MS
#define CONFIG_TELE_CA_UPDATER_BOOT_WAIT_TIMEOUT_MS 60000
#endif

static const char *TAG = "telesystem";

static void ca_updater_boot_task(void *ctx)
{
    (void)ctx;

    if (!CONFIG_TELE_CA_UPDATER_BOOT_ENABLED ||
        CONFIG_TELE_CA_UPDATER_MANIFEST_URL[0] == '\0') {
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Aguardando Wi-Fi para verificar manifest de CA");
    esp_err_t err = wifi_manager_wait_until_ready(pdMS_TO_TICKS(CONFIG_TELE_CA_UPDATER_BOOT_WAIT_TIMEOUT_MS));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CA updater ignorado: Wi-Fi nao ficou pronto (%s)", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    const tele_artifact_request_t request = {
        .artifact_type = "ca_bundle",
        .manifest_url = CONFIG_TELE_CA_UPDATER_MANIFEST_URL,
        .channel = CONFIG_TELE_CA_UPDATER_CHANNEL,
        .restart_on_success = CONFIG_TELE_CA_UPDATER_RESTART_ON_UPDATE,
    };
    tele_artifact_apply_result_t result = {0};
    err = tele_artifacts_apply(&request, &result);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Falha ao aplicar manifest de CA: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG,
                 "Manifest de CA finalizado: result=%d url=%s",
                 result.run.result,
                 result.run.selected_url);
    }

    vTaskDelete(NULL);
}

static void maybe_start_ca_updater_boot_task(void)
{
    if (!CONFIG_TELE_CA_UPDATER_BOOT_ENABLED ||
        CONFIG_TELE_CA_UPDATER_MANIFEST_URL[0] == '\0') {
        return;
    }

    BaseType_t ok = xTaskCreate(ca_updater_boot_task,
                                "ca_updater",
                                8192,
                                NULL,
                                tskIDLE_PRIORITY + 1,
                                NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar task do CA updater");
    }
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
}

void app_main(void)
{
    esp_err_t err;

    tele_portal_logs_init();
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

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(tele_ca_store_init());
    ESP_ERROR_CHECK(firmware_ota_init());
    ESP_ERROR_CHECK(tele_ca_updater_register_artifact());
    ESP_ERROR_CHECK(firmware_ota_register_artifact());
    ESP_ERROR_CHECK(tele_artifacts_register_commands());
    ESP_ERROR_CHECK(tele_portal_core_register_routes(tele_portal_commands_register_routes));
    ESP_ERROR_CHECK(connectivity_controller_start());
    maybe_start_ca_updater_boot_task();
    ESP_ERROR_CHECK(tele_presence_start());
}
