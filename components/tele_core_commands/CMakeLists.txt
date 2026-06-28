idf_component_register(
    SRCS "tele_core_commands.c"
    INCLUDE_DIRS "include"
    REQUIRES tele_commands tele_config tele_status espressif__cjson
)
