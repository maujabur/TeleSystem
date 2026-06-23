idf_component_register(
    SRCS
        "tele_portal_config.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server tele_config tele_portal_core espressif__cjson
)
