idf_component_register(
    SRCS "dial_power.c"
    INCLUDE_DIRS "."
    REQUIRES dial_state
    PRIV_REQUIRES esp_driver_ledc esp_timer dial_haptics)
