idf_component_register(
    SRCS "src/ai_pump_protocol.c"
    INCLUDE_DIRS "include"
    REQUIRES pump_registry
    PRIV_REQUIRES log
)
