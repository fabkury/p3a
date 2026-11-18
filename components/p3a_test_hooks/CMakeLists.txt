# Layer: net (the top layer below main), because test hooks may reach into
# anything. Test-only code: compiles to nothing unless CONFIG_P3A_TEST_HOOKS
# is set, which only sdkconfig.test.defaults does.
set(srcs "")
if(CONFIG_P3A_TEST_HOOKS)
    list(APPEND srcs "p3a_test_board.c" "p3a_test_console.c" "p3a_test_storage.c" "p3a_test_util.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_uart esp_app_format esp_lcd espressif__esp_lcd_touch esp_timer fatfs nvs_flash sdmmc
                  bootloader_support esp_partition p3a_board p3a_storage p3a_util sd_idle_wait sdio_bus slave_ota
)
