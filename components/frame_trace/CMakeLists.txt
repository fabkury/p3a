set(srcs "")
if(CONFIG_P3A_FRAME_TRACE)
    list(APPEND srcs "frame_trace.c" "frame_trace_wraps.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES esp_timer
    PRIV_REQUIRES log sdmmc spi_flash
)

if(CONFIG_P3A_FRAME_TRACE)
    # Link-time wrappers (see frame_trace_wraps.c). INTERFACE so the options
    # reach the final executable link. Absent from release builds.
    target_link_options(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=sdmmc_read_sectors"
        "-Wl,--wrap=sdmmc_write_sectors"
        "-Wl,--wrap=esp_flash_read"
        "-Wl,--wrap=esp_flash_write"
        "-Wl,--wrap=esp_flash_erase_region"
        "-Wl,--wrap=sdmmc_wait_for_idle")
    # Keep the wrapper objects even though nothing references them by name.
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u __wrap_sdmmc_read_sectors" "-u __wrap_sdmmc_write_sectors" "-u __wrap_esp_flash_read" "-u __wrap_esp_flash_write" "-u __wrap_esp_flash_erase_region" "-u __wrap_sdmmc_wait_for_idle")
endif()
