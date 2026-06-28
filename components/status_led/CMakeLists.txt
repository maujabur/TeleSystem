if(CONFIG_STATUS_LED_ENABLED)
    set(STATUS_LED_DRIVER_SRC "status_led.c")
else()
    set(STATUS_LED_DRIVER_SRC "status_led_stub.c")
endif()

idf_component_register(
    SRCS ${STATUS_LED_DRIVER_SRC} "status_led_effects.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_rmt tele_signal
)
