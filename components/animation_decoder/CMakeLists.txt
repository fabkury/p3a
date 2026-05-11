idf_component_register(
    SRCS "webp_animation_decoder.c"
         "png_animation_decoder.c"
         "jpeg_animation_decoder.c"
         "jpeg_animation_decoder_sw.c"
         "idf_jpeg_release_null_fix.c"
    INCLUDE_DIRS "include" "."
    REQUIRES libwebp_decoder espressif__libpng esp_driver_jpeg espressif__libjpeg-turbo config_store debug_http_log
    PRIV_REQUIRES animated_gif_decoder esp_common
)

# IDF v5.5.2 bug workaround: redirect every caller of
# jpeg_release_codec_handle through our NULL-checking wrapper. The -u
# forces the linker to pull idf_jpeg_release_null_fix.c.obj out of the
# component archive even though no other object references it directly.
# Remove when ESP-IDF fixes the function upstream. See the C file's
# header for the full rationale.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=jpeg_release_codec_handle"
    "-Wl,-u,__wrap_jpeg_release_codec_handle"
)

