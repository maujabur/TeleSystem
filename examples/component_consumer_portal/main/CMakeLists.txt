idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_event esp_netif esp_timer tele_portal_core tele_portal_logs
)
