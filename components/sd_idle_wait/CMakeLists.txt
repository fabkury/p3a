idf_component_register(
    SRCS "sd_idle_wait.c"
    REQUIRES sdmmc frame_trace
    PRIV_REQUIRES esp_timer
)

# Replace IDF's sdmmc_wait_for_idle() at link time (GNU ld --wrap). See sd_idle_wait.c.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=sdmmc_wait_for_idle")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u __wrap_sdmmc_wait_for_idle")
