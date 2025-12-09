idf_component_register(
    SRCS
        "p3a_state.c"
        "p3a_touch_router.c"
        "p3a_render.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_common
        freertos
        nvs_flash
    PRIV_REQUIRES
        json
)

