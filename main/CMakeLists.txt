set(srcs
    "app_lcd_p4.c"
    "app_touch.c"
    "p3a_main.c"
    "display_renderer.c"
    "display_fps_overlay.c"
    "display_upscaler.c"
    "animation_player.c"
    "animation_player_render.c"
    "animation_player_loader.c"
    "playback_controller.c"
    "ugfx_ui.c"
)

if(CONFIG_P3A_USB_MSC_ENABLE)
    list(APPEND srcs
        "app_usb.c"
        "usb_descriptors.c"
    )
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include" "."
    REQUIRES driver animation_decoder p3a_board_ep44b waveshare__esp32_p4_wifi6_touch_lcd_4b espressif__esp_lcd_touch app_state config_store http_api json nvs_flash esp_netif esp_hosted tinyusb esp_tinyusb esp_driver_sdmmc spiffs makapix ugfx ota_manager slave_ota channel_manager wifi_manager pico8 p3a_core sdio_bus live_mode debug_http_log
)

# Add compiler definitions
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    FW_VERSION_STRING="${PROJECT_VER}"
    FW_VERSION_MAJOR=${FW_VERSION_MAJOR}
    FW_VERSION_MINOR=${FW_VERSION_MINOR}
    FW_VERSION_PATCH=${FW_VERSION_PATCH}
)
