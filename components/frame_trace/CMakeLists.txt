set(srcs "")
if(CONFIG_P3A_FRAME_TRACE)
    list(APPEND srcs "frame_trace.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES esp_timer
    PRIV_REQUIRES log
)
