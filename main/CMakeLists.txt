idf_component_register(
    SRCS "app_main.c"
    REQUIRES board p3a_hal ui storage net file_transfer graphics_mode sd_ring
)
