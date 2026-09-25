file(GLOB icon_srcs "assets/icons/*.c")

idf_component_register(SRCS "main.cpp" 
                            "SdUsbManager.cpp"
                            ${icon_srcs}
                       INCLUDE_DIRS "."
                       REQUIRES bsp_board_extra lvgl ui_common nvs_flash esp_wifi esp_http_client mbedtls json app_update)