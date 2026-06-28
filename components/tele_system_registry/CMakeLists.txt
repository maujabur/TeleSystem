idf_component_register(
    SRCS "tele_system_registry.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_system heap tele_config tele_status tele_system tele_wifi espressif__cjson
)
