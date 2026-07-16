# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
add_compile_options("-Wno-format")
# Baseline app version (M6 OTA): esp_app_get_description()->version, compared
# against GitHub Releases' tag_name by dial_ota. Bump per release; the tag
# pushed to GitHub ("dial-vX.Y.Z") must match this for the update check to
# ever consider a release "current" once installed.
set(PROJECT_VER "1.0.1")
project(orion-dial)
