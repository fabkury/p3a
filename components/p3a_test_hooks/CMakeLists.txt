# Layer: net (the top layer below main), because test hooks may reach into
# anything. Test-only code: compiles to nothing unless CONFIG_P3A_TEST_HOOKS
# is set, which only sdkconfig.test.defaults does.
set(srcs "")
if(CONFIG_P3A_TEST_HOOKS)
    list(APPEND srcs "p3a_test_board.c" "p3a_test_console.c" "p3a_test_decode.c" "p3a_test_http.c" "p3a_test_library.c" "p3a_test_net.c" "p3a_test_player.c" "p3a_test_playset.c" "p3a_test_settings.c" "p3a_test_storage.c" "p3a_test_util.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES animation_decoder animation_player display_renderer esp_driver_uart esp_app_format esp_lcd espressif__esp_lcd_touch esp_timer fatfs nvs_flash sdmmc
                  bootloader_support esp_http_server esp_netif esp_partition json lwip espressif__esp_wifi_remote esp_wifi p3a_board p3a_library p3a_net play_scheduler source_local p3a_settings p3a_storage p3a_util sd_idle_wait sdio_bus slave_ota
)
