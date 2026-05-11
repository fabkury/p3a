idf_component_register(
    SRCS
        "art_institution.c"
        "art_institution_rate_limit.c"
        "museums/artic.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES
        esp_http_client
        json
        mbedtls
        p3a_core
        config_store
        channel_manager
    WHOLE_ARCHIVE
)
