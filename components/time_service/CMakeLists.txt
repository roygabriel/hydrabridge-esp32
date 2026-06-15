idf_component_register(
    SRCS "src/time_service.c"
    INCLUDE_DIRS "include"
    REQUIRES config_store
    PRIV_REQUIRES esp_netif log
)
