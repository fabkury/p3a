# http_fetch — shared HTTP fetch/download helper.
#
# LAYERING (do not violate): this component sits BELOW all content fetchers.
# It MUST NOT depend on channel_manager, play_scheduler, giphy, art_institution,
# makapix, or show_url — those components depend on the fetchers, so a back-edge
# here would create a dependency cycle. Domain behavior (rate-limit bookkeeping,
# cooperative cancellation, progress, museum/giphy specifics) is injected by
# callers via the callbacks and options in http_fetch_request_t.
# p3a_core is permitted: it sits below the fetchers too (fs_atomic/sd_health).
idf_component_register(
    SRCS "http_fetch.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client mbedtls sdio_bus freertos esp_common p3a_core
)
