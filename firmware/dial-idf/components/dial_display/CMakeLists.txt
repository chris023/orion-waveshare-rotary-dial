idf_component_register(
    SRCS "dial_display.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_driver_spi esp_lcd esp_timer i2c_bsp lcd_touch_bsp lcd_bl_pwm_bsp)
