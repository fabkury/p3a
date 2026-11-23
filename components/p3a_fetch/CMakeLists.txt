# Layer: library. The one HTTP(S) client: every listing and every download
# goes through http_fetch, which owns retries, redirects, rate-limit
# answers, the cap on concurrent TLS sessions and the write to the card.
# Moved from 1.x unchanged but for its error names (p3a_err.h) and the
# file's SHA-256 computed as it is written.
idf_component_register(
    SRCS "http_fetch.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp_http_client mbedtls p3a_storage p3a_util sdio_bus
)
