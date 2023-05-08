
set(PROJECT_NAME "modbus_master")

idf_component_register(SRCS "main.c"
                        INCLUDE_DIRS "."
                        PRIV_REQUIRES   "bytebeam_esp_sdk"
                                        "nvs_flash"
                                        "app_update"
                                        "mqtt"
                                        "esp_http_client"
                                        "esp_https_ota"
                                        "esp_wifi"
                                        "driver"
                                        "json"
                        REQUIRES esp-modbus modbus_common nvs_flash wifi_provisioning)


# set(EXCLUDE_COMPONENTS freemodbus)


include($ENV{IDF_PATH}/tools/cmake/project.cmake)