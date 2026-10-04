# Siren Detector - ESP32-P4 top-level CMake
# Mirrors ESP32-P4-Platform/examples/esp-idf/12_I2SCodec/CMakeLists.txt

cmake_minimum_required(VERSION 3.16)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../ESP32-P4-Platform/config/esp32p4_local_defaults.cmake")
    include(${CMAKE_CURRENT_LIST_DIR}/../ESP32-P4-Platform/config/esp32p4_local_defaults.cmake)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(COMPONENTS main)

project(siren-detector)
