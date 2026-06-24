idf_component_register(
    SRCS "tele_ca_store.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-tls esp_system mbedtls spiffs
)
