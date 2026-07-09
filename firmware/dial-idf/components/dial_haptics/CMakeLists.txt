idf_component_register(
    SRCS "dial_haptics.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES i2c_bsp nvs_flash)
