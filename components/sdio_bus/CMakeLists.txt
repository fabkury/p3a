# Layer: board. Coordinates the SDMMC controller that Wi-Fi (SDIO slot 1,
# through esp_hosted) and the SD card (slot 0) share.
idf_component_register(
    SRCS "sdio_bus.c"
    INCLUDE_DIRS "include"
)
