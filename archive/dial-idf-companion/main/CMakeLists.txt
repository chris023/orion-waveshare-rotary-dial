idf_component_register(
    SRCS "main.c" "bidi_switch_knob.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_uart esp_driver_gpio esp_timer)
