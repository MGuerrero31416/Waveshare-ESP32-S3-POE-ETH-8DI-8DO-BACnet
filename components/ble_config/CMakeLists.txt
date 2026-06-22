idf_component_register(SRCS "ble_config.c"
                       INCLUDE_DIRS "include" "../../main"
                       REQUIRES bt cjson esp_event nvs_flash esp_timer esp_system)
