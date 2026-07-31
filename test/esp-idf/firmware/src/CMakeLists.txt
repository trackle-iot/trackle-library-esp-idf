idf_component_register(
    SRCS
        "main.c"
        "dut_protocol.c"
        "dut_udp.c"
        "dut_cloud_fns.c"
    INCLUDE_DIRS "."
    REQUIRES
        espressif__cjson
        nvs_flash
        esp_driver_uart
        trackle-library-esp-idf
)
