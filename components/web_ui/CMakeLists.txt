idf_component_register(
    SRCS "src/web_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES light_registry command_engine ble_light_client
    PRIV_REQUIRES esp_http_server mqtt_bridge event_log ota_update log
                  ble_scanner
)
