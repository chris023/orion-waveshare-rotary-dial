idf_component_register(
    SRCS "ui_router.c" "ui_screens.c" "scr_connecting.c" "scr_setup.c" "scr_dial.c"
    INCLUDE_DIRS "."
    REQUIRES dial_state
    PRIV_REQUIRES esp_timer dial_haptics)
