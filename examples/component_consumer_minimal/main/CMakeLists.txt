idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash tele_channels tele_config tele_status tele_commands espressif__cjson
)
