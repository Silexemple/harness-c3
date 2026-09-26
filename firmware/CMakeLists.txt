# Harness C3 — ESP-IDF project for ESP32-C3-MINI-1U + GC9A01 240×240 round.
cmake_minimum_required(VERSION 3.16)

# The firmware version comes from version.txt and is linked into the app
# descriptor, so `hello.fw` is read from the RUNNING image
# (esp_app_get_description()->version) and never from a constant — the
# daemon's update-offer decision compares this string (PROTOCOL.md §10.10).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" PROJECT_VER)
string(STRIP "${PROJECT_VER}" PROJECT_VER)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(harness-c3)
