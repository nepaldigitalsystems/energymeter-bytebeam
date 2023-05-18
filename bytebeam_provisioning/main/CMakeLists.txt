idf_component_register(SRCS "app_main.c"
                        REQUIRES log spiffs)

spiffs_create_partition_image(storage ../config_data FLASH_IN_PROJECT)
