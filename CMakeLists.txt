# Fetch trackle-library release from GitHub
include(FetchContent)
set(TRACKLE_LIBRARY_DIR "${CMAKE_CURRENT_LIST_DIR}/trackle-library")

FetchContent_Populate(trackle-library
     URL            "https://github.com/trackle-iot/trackle-library-cpp/releases/download/v3.3.0/trackle-library-cpp-v3.3.0.tar.gz"
     SOURCE_DIR     "${TRACKLE_LIBRARY_DIR}"
)

idf_component_register(SRCS
     "${COMPONENT_DIR}/trackle_esp32.c"
     "${COMPONENT_DIR}/trackle_esp32_cpp.cpp"
     "${COMPONENT_DIR}/trackle-library/src/chunked_transfer.cpp"
     "${COMPONENT_DIR}/trackle-library/src/coap.cpp"
     "${COMPONENT_DIR}/trackle-library/src/coap_channel.cpp"
     "${COMPONENT_DIR}/trackle-library/src/diagnostic.cpp"
     "${COMPONENT_DIR}/trackle-library/src/dtls_message_channel.cpp"
     "${COMPONENT_DIR}/trackle-library/src/dtls_protocol.cpp"
     "${COMPONENT_DIR}/trackle-library/src/events.cpp"
     "${COMPONENT_DIR}/trackle-library/src/trackle.cpp"
     "${COMPONENT_DIR}/trackle-library/src/trackle_interface.cpp"
     "${COMPONENT_DIR}/trackle-library/src/logging.cpp"
     "${COMPONENT_DIR}/trackle-library/src/diagnostic.cpp"
     "${COMPONENT_DIR}/trackle-library/src/messages.cpp"
     "${COMPONENT_DIR}/trackle-library/src/protocol.cpp"
     "${COMPONENT_DIR}/trackle-library/src/protocol_defs.cpp"
     "${COMPONENT_DIR}/trackle-library/src/publisher.cpp"
     "${COMPONENT_DIR}/trackle-library/src/trackle_protocol_functions.cpp"
     "${COMPONENT_DIR}/trackle-library/lib/tinydtls/aes/rijndael.c"
     "${COMPONENT_DIR}/trackle-library/lib/tinydtls/aes/rijndael_wrap.c"
     "${COMPONENT_DIR}/trackle-library/lib/tinydtls/sha2/sha2.c"
     "${COMPONENT_DIR}/trackle-library/lib/tinydtls/ccm.c"
     "${COMPONENT_DIR}/trackle-library/lib/tinydtls/crypto.c"
     "${COMPONENT_DIR}/trackle-library/lib/tinydtls/dtls.c"
     "${COMPONENT_DIR}/trackle-library/lib/tinydtls/dtls_debug.c"
     "${COMPONENT_DIR}/trackle-library/lib/tinydtls/dtls_prng.c"
     "${COMPONENT_DIR}/trackle-library/lib/tinydtls/dtls_time.c"
     "${COMPONENT_DIR}/trackle-library/lib/tinydtls/hmac.c"
     "${COMPONENT_DIR}/trackle-library/lib/tinydtls/netq.c"
     "${COMPONENT_DIR}/trackle-library/lib/tinydtls/peer.c"
     "${COMPONENT_DIR}/trackle-library/lib/tinydtls/session.c"
     "${COMPONENT_DIR}/trackle-library/lib/micro-ecc/uECC.c"
     "${COMPONENT_DIR}/src/trackle_utils.c"
     "${COMPONENT_DIR}/src/trackle_utils_bt_functions.c"
     "${COMPONENT_DIR}/src/trackle_utils_bt_provision.c"
     "${COMPONENT_DIR}/src/trackle_utils_claimcode.c"
    
     INCLUDE_DIRS "." "./trackle-library/include" "./trackle-library/lib/tinydtls" "./trackle-library/lib/tinydtls/aes" "./trackle-library/lib/tinydtls/sha2" "./trackle-library/lib/micro-ecc"
     REQUIRES json wifi_provisioning)

target_link_libraries(${COMPONENT_TARGET} "-u custom_app_desc")
target_compile_definitions(${COMPONENT_TARGET} PUBLIC "-DWITH_ESPIDF")
