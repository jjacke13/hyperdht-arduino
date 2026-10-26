cmake_minimum_required(VERSION 3.20)
set(HYPERDHT_DIR "$ENV{HYPERDHT_DIR}" CACHE PATH "Embedded HyperDHT checkout")
set(HOLESAIL_DIR "$ENV{HOLESAIL_DIR}" CACHE PATH "Holesail codec checkout")
if(NOT EXISTS "${HYPERDHT_DIR}/components/hyperdht/CMakeLists.txt" OR
   NOT EXISTS "${HOLESAIL_DIR}/src/record.cpp")
    message(FATAL_ERROR "Set HYPERDHT_DIR and HOLESAIL_DIR; see README.md")
endif()
set(EXTRA_COMPONENT_DIRS "${HYPERDHT_DIR}/components" "${CMAKE_CURRENT_LIST_DIR}/../..")
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(arduino-p2p VERSION 0.1.0)
idf_component_get_property(arduino_target espressif__arduino-esp32 COMPONENT_LIB)
# Genesis Mini USB connector uses the S3 hardware CDC/JTAG console.
target_compile_definitions(${arduino_target} PUBLIC ARDUINO_USB_MODE=1 ARDUINO_USB_CDC_ON_BOOT=1)
