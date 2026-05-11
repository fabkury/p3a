idf_component_register(
    SRCS
        "art_institution.c"
        "art_institution_rate_limit.c"
        "art_institution_refresh.c"
        "art_institution_resolve.c"
        "art_institution_download.c"
        "museums/common.c"
        "museums/artic.c"
        "museums/rijksmuseum.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS
        "."
        "${CMAKE_CURRENT_SOURCE_DIR}/../../main/include"
    REQUIRES
        esp_http_client
        json
        mbedtls
        p3a_core
        config_store
        channel_manager
        play_scheduler
        makapix
    WHOLE_ARCHIVE
)

# Make FW_VERSION_STRING visible to museums/artic.c (used in AIC-User-Agent).
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    FW_VERSION_STRING="${PROJECT_VER}"
)
