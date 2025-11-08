set(srcs
    "app_lcd_p4.c"
    "app_touch.c"
    "p3a_main.c"
    "animation_player.c"
    "webp_animation_decoder.c"
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES libwebp_decoder waveshare__esp32_p4_wifi6_touch_lcd_4b espressif__esp_lcd_touch animated_gif_decoder
    EMBED_FILES
        idea11-ionic-connections-128p-L92-b809.webp
        decoded_3826495.webp
        r3-artificer-32p-L55.webp
        ms-black-dragon-64p-L82.webp
)
