idf_component_register(
    SRCS "tele_artifacts.c"
    INCLUDE_DIRS "include"
    REQUIRES tele_manifest tele_commands espressif__cjson
)
