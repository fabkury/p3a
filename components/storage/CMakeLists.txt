idf_component_register(
    SRCS "src/storage.c" "src/kv.c" "src/fs.c" "src/cache.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash spiffs fatfs driver board mbedtls
)
