idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_netif esp_event esp_wifi nvs_flash tele_config tele_status tele_commands tele_mqtt espressif__cjson
)
