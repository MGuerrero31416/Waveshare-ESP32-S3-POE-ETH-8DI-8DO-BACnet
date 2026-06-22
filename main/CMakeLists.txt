set(MAIN_SRCS
    "buzzer.c"
    "binary_output.c"
    "binary_input.c"
    "analog_input.c"
    "binary_value.c"
    "analog_value.c"
    "bacnet_system_fsm.c"
    "main.c"
    "wifi_helper.c"
    "mstp_rs485.c"
    "User_Settings.c"
    "waveshare_io.c"
    "bacnet_io_link.c"
    "rgb_status.c")

set(MAIN_REQUIRES
    bacnet-stack
    ethernet_w5500
    led_strip
    cjson)

idf_component_register(SRCS ${MAIN_SRCS}
                       REQUIRES ${MAIN_REQUIRES}
                       PRIV_REQUIRES spi_flash nvs_flash esp_event esp_wifi esp_netif driver esp_timer
                       INCLUDE_DIRS "")

# Add linker flags for Arduino log wrapper compatibility
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_log_write"
    "-Wl,--wrap=esp_log_writev"
)

# Force the linker to include Arduino log wrapper symbols
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--undefined=__wrap_esp_log_write"
    "-Wl,--undefined=__wrap_esp_log_writev"
)
