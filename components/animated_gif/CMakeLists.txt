# Layer: player. Third-party: Larry Bank's AnimatedGIF library, as vendored by
# p3a 1.x. p3a's own GIF code is components/animation_decoder/gif_animation_decoder.cpp.
idf_component_register(
    SRCS "AnimatedGIF.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES esp_timer
)

# gif.inl defines readFLASH for flash-backed builds that this configuration
# does not select.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function)
