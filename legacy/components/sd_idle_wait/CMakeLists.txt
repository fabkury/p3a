# sd_idle_wait: yielding replacement for IDF's sdmmc_wait_for_idle() (jitter fix 8).
# CONFIG_P3A_SD_IDLE_WAIT_WRAP (default y) links the wrapper; when it is off the
# stock IDF function runs and only the identity helpers (sd_idle_wait_info.c)
# are compiled, so a build can always report which variant it carries.
set(srcs "sd_idle_wait_info.c")
if(CONFIG_P3A_SD_IDLE_WAIT_WRAP)
    list(APPEND srcs "sd_idle_wait.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES sdmmc frame_trace
    PRIV_REQUIRES esp_timer
)

if(CONFIG_P3A_SD_IDLE_WAIT_WRAP)
    # Replace IDF's sdmmc_wait_for_idle() at link time (GNU ld --wrap). See sd_idle_wait.c.
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=sdmmc_wait_for_idle")
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u __wrap_sdmmc_wait_for_idle")
endif()
