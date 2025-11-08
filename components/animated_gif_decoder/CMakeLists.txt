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
)
