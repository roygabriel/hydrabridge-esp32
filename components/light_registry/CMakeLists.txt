idf_component_register(
    SRCS "src/light_registry.c"
    INCLUDE_DIRS "include"
    REQUIRES channel_model
    PRIV_REQUIRES nvs_flash log
)
