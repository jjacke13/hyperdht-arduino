cmake_minimum_required(VERSION 3.21)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
project(hyperdht_arduino_native C CXX)

foreach(required ARDUINO_SDK HYPERDHT_DIR HOLESAIL_DIR SODIUM_DIR)
    if(NOT IS_DIRECTORY "${${required}}")
        message(FATAL_ERROR "Set ${required} to its pinned source/SDK directory")
    endif()
endforeach()
set(IDF_TARGET esp32s3)
set(CMAKE_C_FLAGS "@${ARDUINO_SDK}/flags/c_flags")
set(CMAKE_CXX_FLAGS "@${ARDUINO_SDK}/flags/cpp_flags")
get_filename_component(WRAPPER_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
add_compile_options(-Os "@${ARDUINO_SDK}/flags/defines"
    -iprefix "${ARDUINO_SDK}/include/" "@${ARDUINO_SDK}/flags/includes"
    "-I${ARDUINO_SDK}/qio_qspi/include"
    "-ffile-prefix-map=${HYPERDHT_DIR}=hyperdht-cpp"
    "-ffile-prefix-map=${HOLESAIL_DIR}=holesail-cpp"
    "-ffile-prefix-map=${SODIUM_DIR}=libsodium"
    "-ffile-prefix-map=${WRAPPER_DIR}=hyperdht-arduino")
include_directories("${HYPERDHT_DIR}/include"
    "${HYPERDHT_DIR}/components/libuv-esp32/include"
    "${HYPERDHT_DIR}/deps/libudx/include"
    "${SODIUM_DIR}/libsodium/src/libsodium/include" "${SODIUM_DIR}/port_include")

# Reuse the pinned component manifests and their UDX hardening/compile flags.
function(idf_component_register)
    cmake_parse_arguments(C "" "" "SRCS;INCLUDE_DIRS;PRIV_INCLUDE_DIRS;REQUIRES;PRIV_REQUIRES" ${ARGN})
    get_filename_component(name "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
    if(CMAKE_CURRENT_SOURCE_DIR STREQUAL SODIUM_DIR)
        set(name libsodium)
    endif()
    add_library(${name} STATIC ${C_SRCS})
    target_include_directories(${name} PRIVATE ${C_INCLUDE_DIRS} ${C_PRIV_INCLUDE_DIRS})
    set(COMPONENT_LIB ${name} PARENT_SCOPE)
endfunction()
add_subdirectory("${HYPERDHT_DIR}/components/libuv-esp32" uv)
add_subdirectory("${HYPERDHT_DIR}/components/libudx" udx)
add_subdirectory("${SODIUM_DIR}" sodium)
add_subdirectory("${HYPERDHT_DIR}/components/hyperdht" dht)
add_library(hyperdht_native STATIC
    "${WRAPPER_DIR}/src/HyperDHTNode.cpp" "${WRAPPER_DIR}/src/HolesailWebServer.cpp"
    "${WRAPPER_DIR}/src/detail/http.c"
    "${HOLESAIL_DIR}/src/keys.cpp" "${HOLESAIL_DIR}/src/record.cpp"
    $<TARGET_OBJECTS:hyperdht> $<TARGET_OBJECTS:libuv-esp32>
    $<TARGET_OBJECTS:libudx> $<TARGET_OBJECTS:libsodium>)
target_include_directories(hyperdht_native PRIVATE "${WRAPPER_DIR}/src" "${HOLESAIL_DIR}/include")
target_compile_options(hyperdht_native PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20;-fno-exceptions;-fno-rtti>)
