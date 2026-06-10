if(CONFIG_STATUS_LED_ENABLED)
    set(STATUS_LED_SRCS "status_led.c")
else()
    set(STATUS_LED_SRCS "status_led_stub.c")
endif()

idf_component_register(
    SRCS ${STATUS_LED_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_rmt
)
