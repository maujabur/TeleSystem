set(MAIN_SRCS
    "main.c"
    "app_indicators.c"
    "connectivity/connectivity_controller.c"
)

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS "." "connectivity"
    REQUIRES esp_wifi esp_event esp_psram nvs_flash esp_netif status_led tele_artifacts tele_boot_config_button tele_ca_store tele_ca_updater tele_indicator tele_portal tele_portal_commands tele_presence tele_system tele_time_sync tele_wifi tele_wifi_device_config
)
