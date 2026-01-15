// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "loader_service.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <errno.h>

static const char *TAG = "loader_service";

#define SD_READ_CHUNK_SIZE (64 * 1024)
#define SD_READ_MAX_RETRIES 3
#define SD_READ_RETRY_DELAY_MS 50

static esp_err_t read_file_to_buffer(const char *filepath, uint8_t **data_out, size_t *size_out)
{
    if (!filepath || !data_out || !size_out) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        return ESP_FAIL;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        ESP_LOGE(TAG, "Invalid file size: %ld", file_size);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buffer = (uint8_t *)heap_caps_malloc((size_t)file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        buffer = (uint8_t *)malloc((size_t)file_size);
        if (!buffer) {
            ESP_LOGE(TAG, "Failed to allocate %ld bytes for animation file", file_size);
            fclose(f);
            return ESP_ERR_NO_MEM;
        }
    }

    size_t total_read = 0;
    size_t remaining = (size_t)file_size;
    int retry_count = 0;

    while (remaining > 0) {
        size_t chunk_size = (remaining < SD_READ_CHUNK_SIZE) ? remaining : SD_READ_CHUNK_SIZE;
        size_t bytes_read = fread(buffer + total_read, 1, chunk_size, f);

        if (bytes_read == 0) {
            if (ferror(f)) {
                if (retry_count < SD_READ_MAX_RETRIES) {
                    retry_count++;
                    ESP_LOGW(TAG, "SD read error at offset %zu, retry %d/%d",
                             total_read, retry_count, SD_READ_MAX_RETRIES);
                    clearerr(f);
                    vTaskDelay(pdMS_TO_TICKS(SD_READ_RETRY_DELAY_MS * retry_count));
                    continue;
                }
                ESP_LOGE(TAG, "SD read failed after %d retries at offset %zu",
                         SD_READ_MAX_RETRIES, total_read);
                fclose(f);
                free(buffer);
                return ESP_ERR_INVALID_SIZE;
            }
            ESP_LOGE(TAG, "Unexpected EOF: read %zu of %ld bytes", total_read, file_size);
            fclose(f);
            free(buffer);
            return ESP_ERR_INVALID_SIZE;
        }

        total_read += bytes_read;
        remaining -= bytes_read;
        retry_count = 0;

        if (remaining > 0 && bytes_read == chunk_size) {
            taskYIELD();
        }
    }

    fclose(f);

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
