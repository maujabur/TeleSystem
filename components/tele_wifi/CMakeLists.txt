idf_component_register(
    SRCS
        "boot_config_button.c"
        "device_config_store.c"
        "time_sync.c"
        "wifi_config.c"
        "wifi_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event esp_netif esp_wifi esp_driver_gpio esp_rom lwip nvs_flash
)
