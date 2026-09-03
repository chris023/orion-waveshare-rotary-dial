idf_component_register(
    SRCS "dial_battery.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_adc dial_state)
