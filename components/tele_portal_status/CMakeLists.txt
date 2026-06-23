idf_component_register(
    SRCS
        "tele_portal_status.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server tele_portal_core tele_status tele_system tele_wifi espressif__cjson
)
