# Layer: ui. The SD card offered to a computer as a USB drive. It sits this
# high because a session takes the card away from everything below: it
# stops the scheduler and puts a screen up. usb_session.c is pure and also
# compiled by the host tests; the descriptors and tusb_config.h are 1.x's
# without the PICO-8 interface.
idf_component_register(
    SRCS "p3a_usb.c"
         "usb_descriptors.c"
         "usb_session.c"
    INCLUDE_DIRS "include" "."
    PRIV_REQUIRES esp_timer p3a_board p3a_storage p3a_util play_scheduler sdmmc
)
