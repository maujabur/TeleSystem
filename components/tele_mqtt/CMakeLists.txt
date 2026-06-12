idf_component_register(
    SRCS "tele_mqtt.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event esp_wifi esp_timer mbedtls mqtt tele_config espressif__cjson
)
