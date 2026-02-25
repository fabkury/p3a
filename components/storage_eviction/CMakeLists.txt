idf_component_register(
    SRCS "storage_eviction.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common p3a_core wifi_manager fatfs
)
