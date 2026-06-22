idf_component_register(
    SRCS "w5500_eth.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_eth esp_netif driver log freertos esp_event
)
