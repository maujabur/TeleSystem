set(MAIN_SRCS
    "main.c"
    "connectivity/connectivity_controller.c"
    "connectivity/device_config_routes.c"
)

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS "." "connectivity"
    REQUIRES esp_wifi esp_event esp_psram nvs_flash esp_netif status_led tele_portal tele_presence tele_system tele_wifi espressif__cjson
)
