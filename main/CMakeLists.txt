set(srcs
    "app_lcd_p4.c"
    "app_touch.c"
    "p3a_main.c"
    "display_renderer.c"
    "display_fps_overlay.c"
    "display_processing_notification.c"
    "display_upscaler.c"
    "render_engine.c"
    "content_service.c"
    "playback_service.c"
    "connectivity_service.c"
    "animation_player.c"
    "animation_player_render.c"
    "animation_player_loader.c"
    "playback_controller.c"
    "ugfx_ui.c"
    "display_reaction_overlay.c"
    "reaction_submit_img.c"
    "reaction_revoke_img.c"
    "reaction_error_img.c"
    "submit_click_img.c"
    "display_pin_overlay.c"
    "pin_icon_img.c"
    "unpin_icon_img.c"
    "ffsystem_aligned.c"
)

if(CONFIG_P3A_PPA_UPSCALE_ENABLE)
    list(APPEND srcs
        "display_ppa_upscaler.c"
    )
endif()

if(CONFIG_P3A_USB_MSC_ENABLE)
    list(APPEND srcs
        "app_usb.c"
        "usb_descriptors.c"
    )
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include" "."
    REQUIRES driver animation_decoder p3a_board_ep44b waveshare__esp32_p4_wifi6_touch_lcd_4b espressif__esp_lcd_touch config_store http_api json nvs_flash esp_netif esp_hosted tinyusb esp_tinyusb esp_driver_sdmmc makapix ugfx ota_manager slave_ota channel_manager wifi_manager pico8 p3a_core sdio_bus debug_http_log play_scheduler event_bus content_cache playback_queue loader_service show_url giphy art_institution esp_driver_ppa storage_eviction pin_lists mem_stats
    PRIV_REQUIRES esp_mm
)

# Version/API compile definitions are injected globally in the root
# CMakeLists.txt (add_compile_definitions before project()), so no
# per-component declaration is needed here.

# Redirect FATFS's allocator (ff_memalloc/ff_memfree) to our cache-aligned
# implementation in ffsystem_aligned.c. Without this the SDMMC driver bounces
# every PSRAM-backed FATFS buffer through a 4 KB internal-RAM allocation per
# transfer; under fragmentation that allocation fails with ESP_ERR_NO_MEM and
# every SD op cascades into "sdmmc_read_sectors: not enough mem".
#
# -u forces the linker to treat the wrap symbols as initially-unresolved so it
# pulls ffsystem_aligned.c.obj out of libmain.a. Without -u, the IDF link line
# scans libmain.a before libfatfs.a creates the __wrap_ff_* references and the
# object is skipped, producing "undefined reference to __wrap_ff_memalloc".
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=ff_memalloc"
    "-Wl,--wrap=ff_memfree"
    "-Wl,-u,__wrap_ff_memalloc"
    "-Wl,-u,__wrap_ff_memfree"
)
