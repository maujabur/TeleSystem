idf_component_register(
    SRCS "tele_config.c" "tele_config_json.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__cjson nvs_flash tele_channels
)
