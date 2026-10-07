idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES dial_display dial_state dial_ui dial_net dial_oauth dial_mcp dial_knob dial_time dial_haptics dial_power dial_ota esp_timer cjson nvs_flash mdns)

# Dev-only switch (see DIAL_DEMO_LAPSED in main.c): forces the calm "Sign-in
# expired" screen on an unlinked dial. Pass it to a SEPARATE build dir only —
#   idf.py -B build-demo -DDIAL_DEMO_LAPSED=1 build
# — so the cached variable can never leak into a normal or release build.
if(DIAL_DEMO_LAPSED)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE DIAL_DEMO_LAPSED=1)
endif()
