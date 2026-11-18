# Layer: net. Wi-Fi through the ESP32-C6 co-processor, the device's name on
# the LAN, the clock, and the HTTP server with the built-in /api/v2 endpoints.
# The setup portal joins it later in M1.5.
#
# net_backoff.h, http_route.c and http_envelope.c have no IDF includes and are
# also compiled by the host tests.
idf_component_register(
    SRCS "http_envelope.c" "http_route.c" "http_server.c"
         "net_api.c"
         "net_link_recovery.c"
         "net_time.c"
         "net_wifi.c"
         "net_wifi_recovery.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server json p3a_util
    PRIV_REQUIRES esp_app_format esp_event esp_partition espcoredump p3a_storage esp_netif esp_timer esp_wifi frame_trace lwip nvs_flash p3a_settings p3a_util
                  espressif__esp_hosted espressif__esp_wifi_remote espressif__mdns
)
