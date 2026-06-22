# Fetch trackle-library release from GitHub

if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/.trackle_developer")
     include(FetchContent)
     set(TRACKLE_LIBRARY_DIR "${CMAKE_CURRENT_LIST_DIR}/trackle-library")

     FetchContent_Populate(trackle-library
          URL "https://github.com/trackle-iot/trackle-library-cpp/releases/download/v4.4.0/trackle-library-cpp-v4.4.0.tar.gz"
          SOURCE_DIR "${TRACKLE_LIBRARY_DIR}"
     )
endif()

# ---------------------------------------------------------------------------
# Stack libcoap: LibcoapChannel + LibcoapProtocol
#
# Requisiti sdkconfig:
#   CONFIG_COAP_DTLS_SUPPORT=y
#   CONFIG_COAP_MBEDTLS_ENCRYPTION=y   (ESP-IDF 5+, consigliato)
#     oppure CONFIG_COAP_TINYDTLS_ENCRYPTION=y
#   CONFIG_COAP_MBEDTLS_PKI_SUPPORT=y
# ---------------------------------------------------------------------------

set(TRACKLE_SRCS
     "${COMPONENT_DIR}/trackle_esp32.c"
     "${COMPONENT_DIR}/trackle_esp32_cpp.cpp"
     "${COMPONENT_DIR}/trackle-library/src/coap.cpp"
     "${COMPONENT_DIR}/trackle-library/src/diagnostic.cpp"
     "${COMPONENT_DIR}/trackle-library/src/trackle.cpp"
     "${COMPONENT_DIR}/trackle-library/src/trackle_interface.cpp"
     "${COMPONENT_DIR}/trackle-library/src/logging.cpp"
     "${COMPONENT_DIR}/trackle-library/src/messages.cpp"
     "${COMPONENT_DIR}/trackle-library/src/protocol.cpp"
     "${COMPONENT_DIR}/trackle-library/src/protocol_defs.cpp"
     "${COMPONENT_DIR}/trackle-library/src/trackle_protocol_functions.cpp"
     "${COMPONENT_DIR}/trackle-library/src/libcoap_channel.cpp"
     "${COMPONENT_DIR}/trackle-library/src/libcoap_protocol.cpp"
     "${COMPONENT_DIR}/src/trackle_utils.c"
     "${COMPONENT_DIR}/src/trackle_utils_bt_functions.c"
     "${COMPONENT_DIR}/src/trackle_utils_bt_provision.c"
     "${COMPONENT_DIR}/src/trackle_utils_claimcode.c"
     "${COMPONENT_DIR}/src/trackle_utils_ota.c"
     "${COMPONENT_DIR}/src/trackle_utils_storage.c"
     "${COMPONENT_DIR}/src/trackle_utils_wifi.c"
     "${COMPONENT_DIR}/src/trackle_utils_crypto.c"
)

idf_component_register(
     SRCS ${TRACKLE_SRCS}
     INCLUDE_DIRS "." "./trackle-library/include"
     REQUIRES efuse mbedtls nvs_flash esp_system app_update
              esp_http_client esp_https_ota wifi_provisioning
              coap wolfssl
)

target_link_libraries(${COMPONENT_TARGET} "-u custom_app_desc")
target_compile_definitions(${COMPONENT_TARGET} PUBLIC
    "-DWITH_ESPIDF"
    "-DUSE_LIBCOAP"
    "-DCOAP_WITH_LIBWOLFSSL=1"
    "-DWOLFSSL_USER_SETTINGS"
)

# Fix per il conflitto C++17 std::byte vs wolfSSL byte typedef.
# Compilando i file C++ in C++14 si elimina std::byte (introdotto in C++17),
# risolvendo l'ambiguità con il typedef wolfSSL senza toccare la libreria.
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++14>
)
