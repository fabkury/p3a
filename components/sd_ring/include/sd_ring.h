#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize SD ring buffer
 * 
 * Allocates a ring buffer in PSRAM with the specified chunk size and count.
 * Chunks are cache-line aligned (64-byte).
 * 
 * @param chunk_size Size of each chunk in bytes (should be 128-256 KiB, aligned to SDMMC block size)
 * @param num_chunks Number of chunks (2-4 recommended)
 * @return ESP_OK on success
 */
esp_err_t sd_ring_init(size_t chunk_size, size_t num_chunks);

/**
 * @brief Open a file for reading via ring buffer
 * 
 * Opens the file and starts prefetching. The reader task will continuously
 * prefetch data ahead of the current read position.
 * 
 * @param path File path (must be on SD card)
 * @return ESP_OK on success
 */
esp_err_t sd_ring_open_file(const char* path);

/**
 * @brief Read data from ring buffer at specified offset
 * 
 * This function reads from the ring buffer, which is continuously filled
 * by the prefetch task. If the requested data is not yet in the buffer,
 * it will trigger a blocking fill.
 * 
 * @param offset File offset in bytes
 * @param buf Destination buffer
 * @param len Number of bytes to read
 * @return Number of bytes read, or -1 on error
 */
ssize_t sd_ring_read_at(off_t offset, void* buf, size_t len);

/**
 * @brief Get file size
 * 
 * @return File size in bytes, or -1 if no file is open
 */
ssize_t sd_ring_get_file_size(void);

/**
 * @brief Close current file and stop prefetching
 */
void sd_ring_close(void);

/**
 * @brief Deinitialize ring buffer (free memory)
 */
void sd_ring_deinit(void);

#ifdef __cplusplus
}
#endif

