# Layer: net. Wi-Fi through the ESP32-C6 co-processor, the device's name on
# the LAN, and the clock. The HTTP server and the setup portal join it later
# in M1.5.
#
# net_backoff.h has no IDF includes and is also compiled by the host tests.
idf_component_register(
    SRCS "net_link_recovery.c"
         "net_time.c"
         "net_wifi.c"
         "net_wifi_recovery.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_event esp_netif esp_timer esp_wifi frame_trace lwip nvs_flash p3a_settings p3a_util
                  espressif__esp_hosted espressif__esp_wifi_remote espressif__mdns
)
