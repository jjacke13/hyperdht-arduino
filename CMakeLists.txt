idf_component_register(
    SRCS "src/HyperDHTNode.cpp" "src/HolesailWebServer.cpp" "src/detail/http.c"
         "${HOLESAIL_DIR}/src/keys.cpp" "${HOLESAIL_DIR}/src/record.cpp"
    INCLUDE_DIRS "src"
    PRIV_INCLUDE_DIRS "${HOLESAIL_DIR}/include"
    REQUIRES hyperdht arduino-esp32
    PRIV_REQUIRES esp_timer esp_netif)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20;-fno-exceptions;-fno-rtti>)
if(NOT CONFIG_DIAG_USE_EXTERNAL_LOG_WRAP)
    # Pull Arduino's log wrappers from its archive before late Wi-Fi references.
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u __wrap_esp_log_write" "-u __wrap_esp_log_writev")
endif()
