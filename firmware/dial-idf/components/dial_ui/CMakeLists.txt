idf_component_register(
    SRCS "ui_router.c"
    INCLUDE_DIRS "."
    REQUIRES dial_state
    PRIV_REQUIRES esp_timer)
