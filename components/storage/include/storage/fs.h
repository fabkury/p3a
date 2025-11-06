#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @brief Filesystem status information
 */
typedef struct {
    bool spiffs_mounted;
    bool sd_mounted;
    uint64_t sd_total_bytes;
    uint64_t sd_free_bytes;
} storage_fs_status_t;

/**
 * @brief Initialize filesystems (SPIFFS on flash partition + SDMMC).
 * 
 * This function mounts SPIFFS from the "storage" partition and sets up
 * SDMMC hot-plug monitoring.
 */
esp_err_t storage_fs_init(void);

/**
 * @brief Get filesystem status.
 */
esp_err_t storage_fs_get_status(storage_fs_status_t *status);

/**
 * @brief Get SPIFFS mount point path.
 */
const char *storage_fs_get_spiffs_path(void);

/**
 * @brief Get SD card mount point path.
 */
const char *storage_fs_get_sd_path(void);

/**
 * @brief Check if SD card is currently inserted.
 */
bool storage_fs_is_sd_present(void);

