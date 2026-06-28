idf_component_register(
    SRCS
        "wifi_config.c"
        "wifi_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event esp_netif esp_wifi lwip nvs_flash
)
