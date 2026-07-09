# Minimal ESP32-S3 firmware that streams the raw levels + interrupt edge counts
# of the rotary-encoder pins (schematic EC1: A=GPIO8, B=GPIO7), plus a scan of
# a few free neighbor GPIOs. Definitive test of whether the knob is electrically
# reaching the S3 at all. Flash to the S3 (native USB), turn slowly, watch serial.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(knob-probe)
