idf_component_register(
    SRCS
        "web_portal.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_system tele_firmware_portal_ota tele_portal_assets tele_portal_captive tele_portal_config tele_portal_core tele_portal_logs tele_portal_status tele_portal_wifi
)
