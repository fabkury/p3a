# Layer: board.
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

# Does this IDF carry Espressif's own fix (esp-idf #19034)? It adds
# sdmmc_poll_delay_and_backoff() to sdmmc_common.c. Decided here, at configure
# time, by reading the IDF source, and reported by sd_idle_wait_idf_patched().
file(STRINGS "$ENV{IDF_PATH}/components/sdmmc/sdmmc_common.c" p3a_backoff_lines REGEX "sdmmc_poll_delay_and_backoff")
if(p3a_backoff_lines)
    set(p3a_idf_has_backoff 1)
else()
    set(p3a_idf_has_backoff 0)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE "P3A_IDF_HAS_SDMMC_BACKOFF=${p3a_idf_has_backoff}")

if(CONFIG_P3A_SD_IDLE_WAIT_WRAP)
    # Replace IDF's sdmmc_wait_for_idle() at link time (GNU ld --wrap). See sd_idle_wait.c.
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=sdmmc_wait_for_idle")
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u __wrap_sdmmc_wait_for_idle")
endif()
