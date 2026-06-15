idf_component_register(
    SRCS "src/schedule_engine.c"
    INCLUDE_DIRS "include"
    REQUIRES config_store command_engine channel_model
    PRIV_REQUIRES freertos log sun_service
)
