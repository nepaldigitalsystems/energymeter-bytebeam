
set(PROJECT_NAME "modbus_master")

idf_component_register(SRCS "main.c"
                        INCLUDE_DIRS "."
                        REQUIRES modbus_common freemodbus nvs_flash)
