idf_component_register(
  SRCS "i2c_bsp.c"
  REQUIRES esp_driver_i2c
  PRIV_REQUIRES dial_display
  INCLUDE_DIRS "./")
