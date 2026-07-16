cmake_minimum_required(VERSION 3.16)
project(dial_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ---- LVGL 8.4.0 --------------------------------------------------------
# firmware/dial-idf pins lvgl/lvgl at exactly 8.4.0 (managed_components/ is
# gitignored, so a fresh clone won't have it). Use that local checkout
# directly when present (fast, offline, guaranteed identical to what the
# firmware builds against); otherwise fetch the same tagged release.
set(LV_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h CACHE STRING "" FORCE)

option(DIAL_SIM_FORCE_FETCH_LVGL "Always FetchContent lvgl/lvgl, ignoring any local managed_components checkout" OFF)

set(DIAL_SIM_LOCAL_LVGL "${CMAKE_CURRENT_SOURCE_DIR}/../firmware/dial-idf/managed_components/lvgl__lvgl")
if(EXISTS "${DIAL_SIM_LOCAL_LVGL}/CMakeLists.txt" AND NOT DIAL_SIM_FORCE_FETCH_LVGL)
  message(STATUS "dial_sim: using local LVGL checkout at ${DIAL_SIM_LOCAL_LVGL}")
  add_subdirectory(${DIAL_SIM_LOCAL_LVGL} ${CMAKE_CURRENT_BINARY_DIR}/lvgl)
else()
  message(STATUS "dial_sim: no local managed_components/lvgl__lvgl checkout; fetching lvgl/lvgl v8.4.0")
  include(FetchContent)
  FetchContent_Declare(lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG        v8.4.0
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(lvgl)
endif()

# ---- the real firmware UI sources --------------------------------------
# HARD RULE: nothing under firmware/ is modified. We compile it as-is,
# adapting the simulator around it via shims/stubs.
set(DIAL_UI_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/../firmware/dial-idf/components/dial_ui")
set(FW_COMPONENTS  "${CMAKE_CURRENT_SOURCE_DIR}/../firmware/dial-idf/components")

set(DIAL_UI_SRCS
  ${DIAL_UI_DIR}/ui_router.c
  ${DIAL_UI_DIR}/ui_screens.c
  ${DIAL_UI_DIR}/scr_connecting.c
  ${DIAL_UI_DIR}/scr_setup.c
  ${DIAL_UI_DIR}/scr_dial.c
  ${DIAL_UI_DIR}/scr_netpick.c
  ${DIAL_UI_DIR}/scr_passkey.c
  ${DIAL_UI_DIR}/scr_menu.c
  ${DIAL_UI_DIR}/scr_tonight.c
  ${DIAL_UI_DIR}/scr_wifi.c
  ${DIAL_UI_DIR}/scr_about.c
  ${DIAL_UI_DIR}/scr_standby.c
  ${DIAL_UI_DIR}/scr_quick.c
  ${DIAL_UI_DIR}/scr_boost.c
  ${DIAL_UI_DIR}/scr_welcome.c
  ${DIAL_UI_DIR}/scr_sidepick.c
  ${DIAL_UI_DIR}/scr_settings.c
  ${DIAL_UI_DIR}/dial_palette.c
  ${DIAL_UI_DIR}/dial_list.c
)
set(DIAL_UI_FONT_SRC ${DIAL_UI_DIR}/dial_font_num_88.c)

# lv_font_conv emits a font source that includes "lvgl.h" directly unless
# told to use the simple include — same fix the real dial_ui/CMakeLists.txt
# applies.
set_source_files_properties(${DIAL_UI_FONT_SRC} PROPERTIES
  COMPILE_OPTIONS -DLV_LVGL_H_INCLUDE_SIMPLE)

# ---- the simulator's own sources ---------------------------------------
set(SIM_SRCS
  main.c
  sim_state.c
  stubs.c
)

add_executable(dial_sim ${SIM_SRCS} ${DIAL_UI_SRCS} ${DIAL_UI_FONT_SRC})

target_include_directories(dial_sim PRIVATE
  # Fake esp_log.h / freertos/*.h / esp_wifi.h / esp_app_desc.h / esp_err.h
  # FIRST, so they win over anything else on the include path.
  ${CMAKE_CURRENT_SOURCE_DIR}/shims
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${FW_COMPONENTS}/dial_state
  ${FW_COMPONENTS}/dial_haptics
  ${FW_COMPONENTS}/dial_power
  ${FW_COMPONENTS}/dial_display
  ${FW_COMPONENTS}/dial_net
  ${FW_COMPONENTS}/dial_ota
  ${DIAL_UI_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/vendor
)

target_link_libraries(dial_sim PRIVATE lvgl::lvgl m)

target_compile_options(dial_sim PRIVATE -Wall -Wno-unused-parameter)

# Where the PNGs land — always the repo's docs/screens/, regardless of the
# build directory's location or the binary's working directory at run time.
get_filename_component(DIAL_SIM_SCREENS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../docs/screens" ABSOLUTE)
target_compile_definitions(dial_sim PRIVATE DIAL_SIM_OUTPUT_DIR="${DIAL_SIM_SCREENS_DIR}")
