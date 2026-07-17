idf_component_register(
    SRCS "ui_router.c" "ui_screens.c" "scr_connecting.c" "scr_setup.c" "scr_dial.c"
         "scr_netpick.c" "scr_passkey.c"
         "scr_menu.c" "scr_tonight.c" "scr_wifi.c" "scr_about.c" "scr_updating.c"
         "scr_standby.c" "scr_quick.c" "scr_boost.c" "scr_welcome.c" "scr_sidepick.c"
         "scr_settings.c" "dial_palette.c" "dial_list.c" "dial_font_num_88.c"
    INCLUDE_DIRS "."
    REQUIRES dial_state
    PRIV_REQUIRES esp_timer dial_haptics dial_power dial_display esp_app_format dial_ota esp_wifi dial_net)

# lv_font_conv emits a font source that includes "lvgl.h" directly (not via
# the project's lv_conf-relative path) unless told to use the simple include.
set_source_files_properties(dial_font_num_88.c PROPERTIES
    COMPILE_OPTIONS -DLV_LVGL_H_INCLUDE_SIMPLE)
