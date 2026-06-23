idf_component_register(
    SRCS
        "tele_portal_logs.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server tele_portal_core
)
