// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 p3a Contributors

/**
 * @file loader_service.c
 * @brief Animation file loader: reads file to PSRAM buffer and initializes decoder
 */

#include "loader_service.h"
#include "p3a_limits.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <errno.h>

static const char *TAG = "loader_service";

#define SD_READ_CHUNK_SIZE (64 * 1024)
#define SD_DMA_ALIGN 128   // ESP32-P4 L2 cache line: direct SD DMA to PSRAM needs this alignment
#define SD_READ_MAX_RETRIES 3
#define SD_READ_RETRY_DELAY_MS 50

static esp_err_t read_file_to_buffer(const char *filepath, uint8_t **data_out, size_t *size_out)
{
    if (!filepath || !data_out || !size_out) {
        return ESP_ERR_INVALID_ARG;
    }

    // Jitter work stream, fix 6 (2026-08-29): POSIX read(), not fread().
    // newlib's stdio layer feeds FATFS one 512-byte sector per SD command
    // whatever the fread() size (the stdio buffer is st_blksize = the sector
    // size, and unbuffered streams read through a 1-byte buffer): a 4.6 MB
    // artwork was 9523 single-sector reads over 5.6 s, and such command storms
    // stalled playback. read() hands the whole 64 KB request to FATFS, which
    // reads whole clusters straight into the aligned PSRAM buffer below.
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        ESP_LOGE(TAG, "Failed to open file: %s (errno=%d)", filepath, errno);
        return ESP_FAIL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        ESP_LOGE(TAG, "fstat failed: %s (errno=%d)", filepath, errno);
        close(fd);
        return ESP_FAIL;
    }
    long file_size = (long)st.st_size;

    if (file_size <= 0) {
        ESP_LOGE(TAG, "Invalid file size: %ld", file_size);
        close(fd);
        return ESP_ERR_INVALID_SIZE;
    }

    if (file_size > P3A_MAX_ARTWORK_SIZE) {
        ESP_LOGW(TAG, "File too large to load: %ld bytes (limit %d)", file_size, P3A_MAX_ARTWORK_SIZE);
        close(fd);
        return ESP_ERR_INVALID_SIZE;
    }

    // Cache-line aligned address AND size: the ESP32-P4 SD host only DMAs
    // directly to a PSRAM buffer that satisfies both; otherwise every 512-byte
    // sector goes through an internal bounce buffer with its own SD command
    // (~1000 commands per 500 KB artwork). See docs/jitter/PLAN.md, H3b.
    const size_t alloc_size = ((size_t)file_size + SD_DMA_ALIGN - 1) & ~(size_t)(SD_DMA_ALIGN - 1);
    uint8_t *buffer = (uint8_t *)heap_caps_aligned_alloc(SD_DMA_ALIGN, alloc_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        buffer = (uint8_t *)malloc((size_t)file_size);
        if (!buffer) {
            ESP_LOGE(TAG, "Failed to allocate %ld bytes for animation file", file_size);
            close(fd);
            return ESP_ERR_NO_MEM;
        }
    }

    size_t total_read = 0;
    size_t remaining = (size_t)file_size;
    int retry_count = 0;

    while (remaining > 0) {
        size_t chunk_size = (remaining < SD_READ_CHUNK_SIZE) ? remaining : SD_READ_CHUNK_SIZE;
        ssize_t got = read(fd, buffer + total_read, chunk_size);

        if (got < 0) {
            if (retry_count < SD_READ_MAX_RETRIES) {
                retry_count++;
                ESP_LOGW(TAG, "SD read error at offset %zu (errno=%d), retry %d/%d",
                         total_read, errno, retry_count, SD_READ_MAX_RETRIES);
                vTaskDelay(pdMS_TO_TICKS(SD_READ_RETRY_DELAY_MS * retry_count));
                continue;
            }
            ESP_LOGE(TAG, "SD read failed after %d retries at offset %zu",
                     SD_READ_MAX_RETRIES, total_read);
            close(fd);
            free(buffer);
            return ESP_ERR_INVALID_SIZE;
        }
        if (got == 0) {
            ESP_LOGE(TAG, "Unexpected EOF: read %zu of %ld bytes", total_read, file_size);
            close(fd);
            free(buffer);
            return ESP_ERR_INVALID_SIZE;
        }
        size_t bytes_read = (size_t)got;

        total_read += bytes_read;
        remaining -= bytes_read;
        retry_count = 0;

        if (remaining > 0 && bytes_read == chunk_size) {
            taskYIELD();
        }
    }

    close(fd);

    if (total_read != (size_t)file_size) {
        ESP_LOGE(TAG, "Failed to read complete file: read %zu of %ld bytes", total_read, file_size);
        free(buffer);
        return ESP_ERR_INVALID_SIZE;
    }

    *data_out = buffer;
    *size_out = (size_t)file_size;
    return ESP_OK;
}

esp_err_t loader_service_load(const char *filepath,
                              animation_decoder_type_t decoder_type,
                              loaded_animation_t *out)
{
    if (!filepath || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    esp_err_t err = read_file_to_buffer(filepath, &out->file_data, &out->file_size);
    if (err != ESP_OK) {
        return err;
    }

    err = animation_decoder_init(&out->decoder, decoder_type, out->file_data, out->file_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize decoder for %s", filepath);
        loader_service_unload(out);
        return err;
    }

    err = animation_decoder_get_info(out->decoder, &out->info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get decoder info for %s", filepath);
        loader_service_unload(out);
        return err;
    }

    // For static formats whose decoder has fully consumed the input bitstream
    // (JPEG, PNG, static WebP - source_consumed=true), the in-memory copy of
    // the file is dead weight for the entire display lifetime of the asset.
    // Free it now and zero the pointer/size so the downstream owner sees an
    // already-released slot. Animated formats (animated WebP, GIF) leave
    // source_consumed=false because their decoders read chunks lazily across
    // subsequent decode_next_* calls; they retain file_data until unload.
    if (out->info.frame_count <= 1 && out->info.source_consumed && out->file_data) {
        free(out->file_data);
        out->file_data = NULL;
        out->file_size = 0;
    }

    return ESP_OK;
}

void loader_service_unload(loaded_animation_t *loaded)
{
    if (!loaded) {
        return;
    }

    animation_decoder_unload(&loaded->decoder);
    if (loaded->file_data) {
        free(loaded->file_data);
        loaded->file_data = NULL;
        loaded->file_size = 0;
    }
    memset(&loaded->info, 0, sizeof(loaded->info));
}
