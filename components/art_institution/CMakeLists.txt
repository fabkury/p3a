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
        "museums/vam.c"
        "museums/wellcome.c"
        "museums/smk.c"
        "museums/ham.c"
        "museums/smithsonian.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS
        "."
        "${CMAKE_CURRENT_SOURCE_DIR}/../../main/include"
    REQUIRES
        esp_http_client
        http_fetch
        json
        mbedtls
        p3a_core
        config_store
        channel_manager
        play_scheduler
        makapix
    WHOLE_ARCHIVE
)

# FW_VERSION_STRING (used in the AIC User-Agent in museums/artic.c) is injected
# globally in the root CMakeLists.txt (add_compile_definitions before project()).
