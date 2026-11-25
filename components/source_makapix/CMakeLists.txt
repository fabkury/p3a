# Layer: sources. Makapix Club: its channels listed into records, and (with
# a registration) this device as one of its players.
idf_component_register(
    SRCS "makapix_link.c"
         "makapix_link_logic.c"
         "makapix_parse.c"
         "makapix_register.c"
         "makapix_renew.c"
         "makapix_rpc.c"
         "makapix_store.c"
         "source_makapix.c"
    INCLUDE_DIRS "include"
    REQUIRES json p3a_library
    PRIV_REQUIRES esp-tls esp_app_format esp_timer mbedtls mqtt nvs_flash p3a_fetch p3a_settings p3a_util
)
