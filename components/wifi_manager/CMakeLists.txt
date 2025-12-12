idf_component_register(
    SRCS "app_wifi.c"
         "sntp_sync.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_wifi_remote esp_event esp_netif nvs_flash esp_http_server lwip espressif__mdns
    PRIV_REQUIRES app_state http_api makapix config_store
)

