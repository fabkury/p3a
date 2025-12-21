idf_component_register(
    SRCS "webp_animation_decoder.c"
         "png_animation_decoder.c"
         "jpeg_animation_decoder.c"
    INCLUDE_DIRS "include" "."
    REQUIRES libwebp_decoder espressif__libpng esp_driver_jpeg config_store debug_http_log
    PRIV_REQUIRES animated_gif_decoder esp_common
)

