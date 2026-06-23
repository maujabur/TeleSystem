idf_component_register(
    SRCS
        "http_helpers.c"
        "tele_portal_core.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server espressif__cjson
)
