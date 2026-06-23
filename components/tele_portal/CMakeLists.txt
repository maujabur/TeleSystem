idf_component_register(
    SRCS
        "app_log_buffer.c"
        "captive_portal.c"
        "dns_server.c"
        "ota_portal.c"
        "web_portal.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES
        "../../firmware_assets/web/index.html"
        "../../firmware_assets/web/status.html"
        "../../firmware_assets/web/settings.html"
        "../../firmware_assets/web/networks.html"
        "../../firmware_assets/web/logs.html"
    REQUIRES esp_http_server esp_system lwip tele_portal_core tele_system tele_wifi espressif__cjson
)
