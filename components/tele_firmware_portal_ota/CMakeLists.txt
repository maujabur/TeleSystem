idf_component_register(
    SRCS "tele_firmware_portal_ota.c"
    INCLUDE_DIRS "include"
    REQUIRES tele_system tele_portal_ota espressif__cjson
)
