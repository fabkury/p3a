#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "storage/kv.h"
#include "storage/fs.h"
#include "storage/cache.h"

/**
 * @brief Storage subsystem status
 */
typedef struct {
    bool kv_initialized;
    bool fs_initialized;
    bool cache_initialized;
    storage_fs_status_t fs_status;
    storage_cache_stats_t cache_stats;
} storage_status_t;

/**
 * @brief Initialize storage subsystem (NVS + filesystems)
 */
esp_err_t storage_init(void);

/**
 * @brief Get storage subsystem status
 */
esp_err_t storage_get_status(storage_status_t *status);

