idf_component_register(
    SRCS "tele_ca_updater.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_system tele_artifacts tele_ca_store tele_manifest
)
