cmake_minimum_required(VERSION 3.16)

set(libwebp_placeholder_source_dir "${CMAKE_CURRENT_BINARY_DIR}/_deps/libwebp_upstream-src")

if(NOT DEFINED CMAKE_SCRIPT_MODE_FILE)
    include(FetchContent)

    set(WEBP_REPO_URL "https://chromium.googlesource.com/webm/libwebp")
    set(WEBP_REPO_TAG "v1.4.0")

    set(WEBP_LINK_STATIC OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        libwebp_upstream
        GIT_REPOSITORY ${WEBP_REPO_URL}
        GIT_TAG ${WEBP_REPO_TAG}
        GIT_SHALLOW TRUE
    )

    set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_CWEBP OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_DWEBP OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_GIF2WEBP OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_IMG2WEBP OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_VWEBP OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_WEBP_JS OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_WEBPINFO OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_LIBWEBPJAVA OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_LIBWEBPMUX OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_WEBPMUX OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
    set(WEBP_ENABLE_SIMD OFF CACHE BOOL "" FORCE)
    set(WEBP_USE_THREAD OFF CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(libwebp_upstream)
else()
    # Script mode (used by idf.py dependency probing) cannot run FetchContent.
    # Provide a best-effort guess for the source directory so that include paths resolve.
    if(NOT DEFINED libwebp_upstream_SOURCE_DIR)
        set(libwebp_upstream_SOURCE_DIR "${libwebp_placeholder_source_dir}")
    endif()
endif()

if(NOT DEFINED libwebp_upstream_SOURCE_DIR)
    set(libwebp_upstream_SOURCE_DIR "${libwebp_placeholder_source_dir}")
endif()

idf_component_register(SRCS "webp_decoder_component.c"
                       INCLUDE_DIRS "${libwebp_upstream_SOURCE_DIR}/src")

if(TARGET webpdecoder)
    target_include_directories(${COMPONENT_LIB} PUBLIC "${libwebp_upstream_SOURCE_DIR}/src")
    target_link_libraries(${COMPONENT_LIB} PUBLIC webpdecoder webpdemux sharpyuv)
endif()

if(DEFINED libwebp_upstream_SOURCE_DIR)
    set_property(DIRECTORY ${libwebp_upstream_SOURCE_DIR} PROPERTY POSITION_INDEPENDENT_CODE OFF)
endif()
if(DEFINED libwebp_upstream_BINARY_DIR)
    set_property(DIRECTORY ${libwebp_upstream_BINARY_DIR} PROPERTY POSITION_INDEPENDENT_CODE OFF)
endif()

foreach(_t IN ITEMS webpdecoder webpdemux webp sharpyuv webpdsp webpdspdecode webputils webputilsdecode webpdecode webpencode)
    if(TARGET ${_t})
        set_property(TARGET ${_t} PROPERTY POSITION_INDEPENDENT_CODE OFF)
        set_property(TARGET ${_t} PROPERTY INTERFACE_POSITION_INDEPENDENT_CODE OFF)
    endif()
endforeach()

foreach(_tool IN ITEMS exampleutil imageioutil imagedec imageenc dwebp cwebp gif2webp img2webp vwebp webpinfo webpmux libwebpmux extras get_disto webp_quality vwebp_sdl)
    if(TARGET ${_tool})
        set_property(TARGET ${_tool} PROPERTY EXCLUDE_FROM_ALL TRUE)
        set_property(TARGET ${_tool} PROPERTY EXCLUDE_FROM_DEFAULT_BUILD TRUE)
    endif()
endforeach()
