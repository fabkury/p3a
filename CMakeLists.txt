# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
add_compile_options(-Wno-ignored-qualifiers)
project(p3a)

# Create SPIFFS image from webui directory
# When PICO-8 is disabled, filter out PICO-8 related assets to reduce binary size
if(CONFIG_P3A_PICO8_ENABLE)
    # PICO-8 enabled: use full webui directory
    spiffs_create_partition_image(storage webui FLASH_IN_PROJECT)
else()
    # PICO-8 disabled: create filtered webui without PICO-8 assets
    set(WEBUI_STAGING_DIR "${CMAKE_BINARY_DIR}/webui_filtered")
    
    # Clean and recreate staging directory
    file(REMOVE_RECURSE "${WEBUI_STAGING_DIR}")
    file(MAKE_DIRECTORY "${WEBUI_STAGING_DIR}")
    file(MAKE_DIRECTORY "${WEBUI_STAGING_DIR}/static")
    
    # Copy static files excluding PICO-8 related assets
    file(GLOB STATIC_FILES "${CMAKE_SOURCE_DIR}/webui/static/*")
    foreach(STATIC_FILE ${STATIC_FILES})
        get_filename_component(FILENAME ${STATIC_FILE} NAME)
        # Exclude PICO-8 related files
        if(NOT FILENAME MATCHES "^(fake08\\.|pico8\\.)")
            file(COPY ${STATIC_FILE} DESTINATION "${WEBUI_STAGING_DIR}/static")
        endif()
    endforeach()
    
    # Do not copy pico8/ directory at all
    
    spiffs_create_partition_image(storage "${WEBUI_STAGING_DIR}" FLASH_IN_PROJECT)
endif()

