idf_component_register(
    SRCS
        "firmware_ota.c"
        "power_good.c"
        "vbat_monitor.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update esp-tls esp_adc esp_driver_gpio esp_http_client esp_https_ota esp_system tele_artifacts tele_manifest
)
