idf_component_register(
    SRCS "src/sun_service.c"
    INCLUDE_DIRS "include"
    REQUIRES config_store
    PRIV_REQUIRES log
)
