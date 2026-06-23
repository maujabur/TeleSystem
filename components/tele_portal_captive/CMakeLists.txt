idf_component_register(
    SRCS
        "dns_server.c"
        "tele_portal_captive.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_netif lwip tele_portal_core
)
