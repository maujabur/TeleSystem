set(MAIN_SRCS
    "main.c"
    "app/firmware_ota.c"
    "app/power_good.c"
    "app/vbat_monitor.c"
    "connectivity/boot_config_button.c"
    "connectivity/connectivity_controller.c"
    "connectivity/device_config_routes.c"
    "connectivity/device_config_store.c"
    "connectivity/mqtt_presence.c"
    "connectivity/time_sync.c"
    "connectivity/wifi_config.c"
    "connectivity/wifi_manager.c"
    "portal/app_log_buffer.c"
    "portal/captive_portal.c"
    "portal/dns_server.c"
    "portal/http_helpers.c"
    "portal/ota_portal.c"
    "portal/web_portal.c"
)

if(CONFIG_STATUS_LED_ENABLED)
    list(APPEND MAIN_SRCS "connectivity/status_led.c")
else()
    list(APPEND MAIN_SRCS "connectivity/status_led_stub.c")
endif()

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS "." "app" "connectivity" "portal"
    EMBED_TXTFILES
        "../firmware_assets/web/index.html"
        "../firmware_assets/web/status.html"
        "../firmware_assets/web/settings.html"
        "../firmware_assets/web/networks.html"
        "../firmware_assets/web/logs.html"
    REQUIRES esp_wifi esp_event nvs_flash esp_netif esp_driver_gpio esp_driver_rmt esp_http_client esp_http_server esp_https_ota bootloader_support app_update lwip mbedtls esp_psram esp_adc tele_mqtt espressif__cjson
)
