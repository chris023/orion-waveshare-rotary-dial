idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES dial_display dial_net dial_oauth dial_mcp dial_knob dial_time esp_timer cjson)
