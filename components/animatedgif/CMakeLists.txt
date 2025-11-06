idf_component_register(
    SRCS
        "src/AnimatedGIF.cpp"
        "src/gif_decoder.cpp"
    INCLUDE_DIRS
        "include"
        "src"
    PRIV_REQUIRES
        esp_timer
        freertos
)

