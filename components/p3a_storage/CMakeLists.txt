# Layer: storage. Everything about files on the SD card: where they live, how
# they are written, whether the card can be trusted, repairing what it damaged,
# and the one container format for binary files.
#
# sd_path.c and p3a_frame.c have no IDF includes and are also compiled by the
# host tests in tests/host.
idf_component_register(
    SRCS "fs_atomic.c"
         "p3a_frame.c" "p3a_frame_file.c"
         "p3a_storage.c"
         "sd_health.c"
         "sd_path.c" "sd_path_fs.c"
         "sd_repair.c"
    INCLUDE_DIRS "include"
    REQUIRES sdmmc
    PRIV_REQUIRES esp_timer fatfs nvs_flash p3a_board p3a_util
)
