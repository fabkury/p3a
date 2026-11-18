# Layer: player. One interface over every image format p3a plays, and the
# five format decoders behind it.
#
# The libraries they use are components of their own: libwebp_decoder,
# libpng (a fork with APNG reading), animated_gif, and the managed
# libjpeg-turbo. JPEG goes to the chip's hardware decoder first.
idf_component_register(
    SRCS "animation_decoder.c"
         "bmp_animation_decoder.c"
         "gif_animation_decoder.cpp"
         "idf_jpeg_release_null_fix.c"
         "jpeg_animation_decoder.c"
         "jpeg_animation_decoder_sw.c"
         "png_animation_decoder.c"
         "webp_animation_decoder.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    PRIV_REQUIRES animated_gif esp_driver_jpeg espressif__libjpeg-turbo libpng libwebp_decoder p3a_settings p3a_util
)

# ESP-IDF bug workaround, still needed in v5.5.4: every caller of
# jpeg_release_codec_handle goes through a wrapper that checks for NULL. The
# -u makes the linker take the wrapper's object file out of the archive
# although nothing refers to it by name. See idf_jpeg_release_null_fix.c;
# remove both when the IDF fixes the function.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=jpeg_release_codec_handle"
    "-Wl,-u,__wrap_jpeg_release_codec_handle"
)

# The managed libjpeg-turbo component refers to itself as idf::libjpeg-turbo
# (its CMakeLists.txt, line 10), but the IDF registers managed components
# under their prefixed name only, idf::espressif__libjpeg-turbo, so that
# reference fails when the build files are generated. Provide the name it
# asks for. Its external build does not need the IDF's include paths (the
# toolchain file supplies the C headers), but the list must not be empty: the
# wrapper builds "-I<dirs>" unconditionally, and a bare "-I" swallows the next
# compiler flag. Any existing folder will do.
# Remove when the IDF creates the short alias, or libjpeg-turbo stops using it.
if(NOT CMAKE_BUILD_EARLY_EXPANSION AND NOT TARGET idf::libjpeg-turbo)
    add_library(idf::libjpeg-turbo INTERFACE IMPORTED GLOBAL)
    set_property(TARGET idf::libjpeg-turbo PROPERTY INCLUDE_DIRECTORIES "${CMAKE_BINARY_DIR}")
endif()
