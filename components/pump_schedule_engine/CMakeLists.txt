idf_component_register(
    SRCS "src/pump_schedule_engine.c"
    INCLUDE_DIRS "include"
    REQUIRES ai_pump_protocol config_store command_queue pump_registry sun_service
    PRIV_REQUIRES log
)
