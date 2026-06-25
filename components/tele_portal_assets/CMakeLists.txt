idf_component_register(
    SRCS
        "tele_portal_assets.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES
        "../../firmware_assets/web/app.css"
        "../../firmware_assets/web/index.html"
        "../../firmware_assets/web/status.html"
        "../../firmware_assets/web/settings.html"
        "../../firmware_assets/web/networks.html"
        "../../firmware_assets/web/logs.html"
    REQUIRES esp_http_server tele_portal_core
)
