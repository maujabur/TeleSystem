idf_component_register(
    SRCS "tele_presence.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event esp_system tele_mqtt tele_system tele_system_registry tele_wifi espressif__cjson
)
