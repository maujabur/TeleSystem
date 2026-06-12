idf_component_register(
    SRCS "tele_config.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__cjson nvs_flash
)
