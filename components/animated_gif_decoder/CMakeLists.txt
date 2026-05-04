cmake_minimum_required(VERSION 3.16)

# Set C++ standard
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

idf_component_register(
    SRCS
        "AnimatedGIF.cpp"
        "gif_animation_decoder.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}"
    REQUIRES animation_decoder config_store
)

# Suppress -Wunused-function for the vendored AnimatedGIF library (gif.inl
# defines readFLASH for AVR/ESP flash builds that aren't selected in our config).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function)
