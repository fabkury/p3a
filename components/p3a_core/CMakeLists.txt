# Build list of sources
set(COMPONENT_SRCS
    "p3a_state.c"
    "p3a_touch_router.c"
    "p3a_render.c"
)

# Add PIE SIMD assembly if enabled
if(CONFIG_P3A_USE_PIE_SIMD)
    list(APPEND COMPONENT_SRCS "pie_memcpy_128.S")
endif()

idf_component_register(
    SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_common
        freertos
        nvs_flash
    PRIV_REQUIRES
        json
)

