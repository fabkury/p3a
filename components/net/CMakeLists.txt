idf_component_register(
    SRCS 
        "src/net.c"
        "src/wifi_provisioning.c"
        "src/wifi_sta.c"
    INCLUDE_DIRS "include"
    REQUIRES storage esp_http_server esp_event esp_netif esp_wifi_remote esp_hosted
    PRIV_REQUIRES esp_wifi_remote
)

# Add injected headers include directory from esp_wifi_remote
# The injected headers are in the idf_tag_X.Y.Z/include/injected directory
# We need to add this so that esp_wifi.h can be found when included by esp_wifi_remote.h
# Since esp_wifi component doesn't exist on ESP32-P4, we need to add the injected headers
# directly to esp_wifi_remote component's include directories
get_filename_component(ESP_WIFI_REMOTE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../managed_components/espressif__esp_wifi_remote" ABSOLUTE)
set(IDF_VER_DIR "${ESP_WIFI_REMOTE_DIR}/idf_tag_${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}")
if(NOT EXISTS "${IDF_VER_DIR}/include/injected")
    set(IDF_VER_DIR "${ESP_WIFI_REMOTE_DIR}/idf_tag_${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}")
endif()
if(EXISTS "${IDF_VER_DIR}/include/injected")
    message(STATUS "net: Adding injected headers from ${IDF_VER_DIR}/include/injected")
    # Add to our component's include directories
    target_include_directories(${COMPONENT_LIB} PUBLIC "${IDF_VER_DIR}/include/injected")
    # Also add to esp_wifi_remote component so it can find esp_wifi.h when processing esp_wifi_remote.h
    idf_component_get_property(wifi_remote_lib esp_wifi_remote COMPONENT_LIB)
    if(wifi_remote_lib)
        target_include_directories(${wifi_remote_lib} PUBLIC "${IDF_VER_DIR}/include/injected")
    endif()
else()
    message(WARNING "net: Could not find injected headers directory")
endif()

# Ensure esp_hosted public headers are available when building this component
set(ESP_HOSTED_REL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../managed_components/espressif__esp_hosted")
if(EXISTS "${ESP_HOSTED_REL_PATH}/host/esp_hosted.h")
    get_filename_component(ESP_HOSTED_PATH "${ESP_HOSTED_REL_PATH}" ABSOLUTE)
    target_include_directories(${COMPONENT_LIB} PUBLIC
        "${ESP_HOSTED_PATH}/host"
        "${ESP_HOSTED_PATH}/host/api/include")
endif()
