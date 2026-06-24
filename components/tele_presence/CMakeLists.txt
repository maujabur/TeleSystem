idf_component_register(
    SRCS "mqtt_presence.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event esp_system heap tele_ca_store tele_ca_updater tele_commands tele_config tele_mqtt tele_status tele_system tele_wifi espressif__cjson
)
