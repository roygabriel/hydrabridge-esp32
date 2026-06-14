idf_component_register(
    SRCS "src/wifi_station.c"
    INCLUDE_DIRS "include"
    # User-supplied wifi_credentials.h lives in main/. Add it to this
    # component's private include path so the #include in wifi_station.c
    # resolves without dragging main into REQUIRES.
    PRIV_INCLUDE_DIRS "${PROJECT_DIR}/main"
    PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash log
                  espressif__mdns
)
