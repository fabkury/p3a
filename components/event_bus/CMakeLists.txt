idf_component_register(
    SRCS "event_bus.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_timer
)
