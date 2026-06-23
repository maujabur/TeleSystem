idf_component_register(
    SRCS "tele_portal_ota.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server tele_portal_core espressif__cjson
)
