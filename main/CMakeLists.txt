set(srcs
    "app_lcd_p4.c"
    "app_touch.c"
    "app_wifi.c"
    "p3a_main.c"
    "animation_player.c"
    "animation_player_render.c"
    "animation_player_loader.c"
    "animation_player_pico8.c"
    "webp_animation_decoder.c"
    "png_animation_decoder.c"
    "jpeg_animation_decoder.c"
)

if(CONFIG_P3A_PICO8_USB_STREAM_ENABLE)
    list(APPEND srcs
        "app_usb.c"
        "pico8_stream.c"
        "usb_descriptors.c"
    )
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include" "."
    REQUIRES driver libwebp_decoder waveshare__esp32_p4_wifi6_touch_lcd_4b espressif__esp_lcd_touch animated_gif_decoder espressif__libpng esp_driver_jpeg app_state config_store http_api esp_wifi esp_wifi_remote esp_http_server mdns json nvs_flash esp_netif esp_hosted tinyusb esp_tinyusb esp_driver_sdmmc
)
