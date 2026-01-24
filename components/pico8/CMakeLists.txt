# PICO-8 Streaming Component
# Handles PICO-8 frame streaming over WebSocket and rendering

if(CONFIG_P3A_PICO8_ENABLE)
    set(pico8_srcs
        "pico8_stream.c"
        "pico8_render.c"
    )
else()
    set(pico8_srcs
        "pico8_stream_stubs.c"
    )
endif()

idf_component_register(
    SRCS ${pico8_srcs}
    INCLUDE_DIRS "include"
    REQUIRES esp_timer freertos p3a_board_ep44b play_scheduler channel_manager
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../main/include"
)

