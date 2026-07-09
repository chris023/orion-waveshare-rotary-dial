# Companion-MCU (classic ESP32) rotary-encoder probe: ISR edge counts + raw
# levels on GPIO19/22 (schematic EC2). Same rigorous method as the S3 knob-probe,
# now applied to the companion's pins. Flash via the companion's USB-UART bridge
# (/dev/cu.usbserial-210, bootloader @ 0x1000) after flipping the USB-C cable.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(knob-probe-companion)
