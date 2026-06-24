set(MAIN_SRCS
    "main.c"
    "connectivity/connectivity_controller.c"
)

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS "." "connectivity"
    REQUIRES esp_wifi esp_event esp_psram nvs_flash esp_netif status_led tele_ca_store tele_ca_updater tele_portal tele_portal_ota tele_presence tele_system tele_wifi espressif__cjson
)
