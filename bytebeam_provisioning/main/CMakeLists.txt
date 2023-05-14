idf_component_register(SRCS "app_main.c"
                        REQUIRES esp_log esp_spiffs)

spiffs_create_partition_image(storage ../config_data FLASH_IN_PROJECT)
