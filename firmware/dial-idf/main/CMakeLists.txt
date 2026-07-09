idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES dial_display dial_state dial_ui dial_net dial_oauth dial_mcp dial_knob dial_time dial_haptics dial_power dial_ota esp_timer cjson nvs_flash)
