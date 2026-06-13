idf_component_register(
    SRCS "src/modbus_interface.c"
         "src/modbus_slave_driver.c"
    INCLUDE_DIRS "include"
    REQUIRES channel_model command_engine command_queue light_registry preset_engine
    PRIV_REQUIRES log driver esp_driver_uart espressif__esp-modbus config_store
)
