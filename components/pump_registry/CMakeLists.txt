idf_component_register(
    SRCS "src/pump_registry.c"
    INCLUDE_DIRS "include"
    REQUIRES light_registry
    PRIV_REQUIRES nvs_flash log
)
