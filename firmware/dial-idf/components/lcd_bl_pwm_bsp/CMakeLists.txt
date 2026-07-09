idf_component_register(
  SRCS "lcd_bl_pwm_bsp.c"
  PRIV_REQUIRES esp_driver_gpio esp_driver_ledc dial_display
  INCLUDE_DIRS "./")
