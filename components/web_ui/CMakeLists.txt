idf_component_register(
    SRCS "src/web_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES ai_pump_protocol light_registry pump_registry command_engine ble_light_client config_store time_service sun_service schedule_engine pump_schedule_engine
    PRIV_REQUIRES esp_http_server mqtt_bridge event_log ota_update log modbus_interface wifi_station
                  ble_scanner
)
