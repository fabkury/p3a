#include "sd_ring.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "sd_ring";

#define CACHE_LINE_SIZE 64
#define SDMMC_BLOCK_SIZE 512
#define MIN_CHUNK_SIZE (128 * 1024)  // 128 KiB minimum
#define MAX_CHUNK_SIZE (256 * 1024)  // 256 KiB maximum
#define MIN_NUM_CHUNKS 2
#define MAX_NUM_CHUNKS 4

typedef struct {
    uint8_t* data;           // Chunk data (PSRAM, cache-line aligned)
    size_t size;             // Chunk size in bytes
    off_t file_offset;       // File offset this chunk represents
    bool valid;              // True if chunk contains valid data
    SemaphoreHandle_t ready; // Semaphore signaled when chunk is ready
} ring_chunk_t;

typedef struct {
    ring_chunk_t* chunks;
    size_t num_chunks;
    size_t chunk_size;
    
    FILE* file;
    off_t file_size;
    const char* file_path;
    
    // Ring buffer state
    size_t read_chunk_idx;   // Current chunk being read from
    off_t read_offset;       // Current read offset within file
    size_t write_chunk_idx;  // Current chunk being written to
    off_t write_offset;      // Current write/prefetch offset
    
    SemaphoreHandle_t ring_mutex;
    TaskHandle_t reader_task;
    bool running;
    bool initialized;
} sd_ring_ctx_t;

static sd_ring_ctx_t s_ctx = {0};

static void sd_reader_task(void* arg);

static size_t align_up(size_t size, size_t align)
{
    return (size + align - 1) & ~(align - 1);
}

esp_err_t sd_ring_init(size_t chunk_size, size_t num_chunks)
{
    if (s_ctx.initialized) {
        ESP_LOGW(TAG, "Ring buffer already initialized");
        return ESP_OK;
    }

    // Validate parameters
    if (chunk_size < MIN_CHUNK_SIZE || chunk_size > MAX_CHUNK_SIZE) {
        ESP_LOGE(TAG, "Invalid chunk size: %zu (must be %d-%d)", chunk_size, MIN_CHUNK_SIZE, MAX_CHUNK_SIZE);
        return ESP_ERR_INVALID_ARG;
    }

    if (num_chunks < MIN_NUM_CHUNKS || num_chunks > MAX_NUM_CHUNKS) {
        ESP_LOGE(TAG, "Invalid num_chunks: %zu (must be %d-%d)", num_chunks, MIN_NUM_CHUNKS, MAX_NUM_CHUNKS);
        return ESP_ERR_INVALID_ARG;
    }

    // Align chunk size to SDMMC block size and cache line
    size_t aligned_chunk_size = align_up(chunk_size, SDMMC_BLOCK_SIZE);
    aligned_chunk_size = align_up(aligned_chunk_size, CACHE_LINE_SIZE);

    s_ctx.chunk_size = aligned_chunk_size;
    s_ctx.num_chunks = num_chunks;
    s_ctx.read_chunk_idx = 0;
    s_ctx.write_chunk_idx = 0;
    s_ctx.read_offset = 0;
    s_ctx.write_offset = 0;
    s_ctx.running = false;
    s_ctx.file = NULL;
    s_ctx.file_path = NULL;
    s_ctx.file_size = -1;

    // Create mutex
    s_ctx.ring_mutex = xSemaphoreCreateMutex();
    if (!s_ctx.ring_mutex) {
        ESP_LOGE(TAG, "Failed to create ring mutex");
        return ESP_ERR_NO_MEM;
    }

    // Allocate chunk structures
    s_ctx.chunks = (ring_chunk_t*)calloc(num_chunks, sizeof(ring_chunk_t));
    if (!s_ctx.chunks) {
        ESP_LOGE(TAG, "Failed to allocate chunk structures");
        vSemaphoreDelete(s_ctx.ring_mutex);
        return ESP_ERR_NO_MEM;
    }

    // Allocate chunk data buffers in PSRAM
    for (size_t i = 0; i < num_chunks; i++) {
        s_ctx.chunks[i].data = (uint8_t*)heap_caps_aligned_alloc(
            CACHE_LINE_SIZE,
            aligned_chunk_size,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        
        if (!s_ctx.chunks[i].data) {
            ESP_LOGE(TAG, "Failed to allocate chunk %zu in PSRAM", i);
            // Cleanup
            for (size_t j = 0; j < i; j++) {
                free(s_ctx.chunks[j].data);
            }
            free(s_ctx.chunks);
            vSemaphoreDelete(s_ctx.ring_mutex);
            return ESP_ERR_NO_MEM;
        }

        s_ctx.chunks[i].size = aligned_chunk_size;
        s_ctx.chunks[i].file_offset = -1;
        s_ctx.chunks[i].valid = false;
        s_ctx.chunks[i].ready = xSemaphoreCreateBinary();
        if (!s_ctx.chunks[i].ready) {
            ESP_LOGE(TAG, "Failed to create semaphore for chunk %zu", i);
            // Cleanup
            for (size_t j = 0; j <= i; j++) {
                free(s_ctx.chunks[j].data);
                if (s_ctx.chunks[j].ready) {
                    vSemaphoreDelete(s_ctx.chunks[j].ready);
                }
            }
            free(s_ctx.chunks);
            vSemaphoreDelete(s_ctx.ring_mutex);
            return ESP_ERR_NO_MEM;
        }
    }

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "SD ring buffer initialized: %zu chunks × %zu bytes = %zu KiB total",
             num_chunks, aligned_chunk_size, (num_chunks * aligned_chunk_size) / 1024);
    
    return ESP_OK;
}

esp_err_t sd_ring_open_file(const char* path)
{
    if (!s_ctx.initialized) {
        ESP_LOGE(TAG, "Ring buffer not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }

    // Close any existing file
    sd_ring_close();

    // Open file
    FILE* f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (file_size < 0) {
        fclose(f);
        ESP_LOGE(TAG, "Failed to get file size");
        return ESP_FAIL;
    }

    if (xSemaphoreTake(s_ctx.ring_mutex, portMAX_DELAY) != pdTRUE) {
        fclose(f);
        return ESP_ERR_TIMEOUT;
    }

    s_ctx.file = f;
    s_ctx.file_size = (off_t)file_size;
    s_ctx.file_path = path;
    s_ctx.read_chunk_idx = 0;
    s_ctx.write_chunk_idx = 0;
    s_ctx.read_offset = 0;
    s_ctx.write_offset = 0;

    // Reset all chunks
    for (size_t i = 0; i < s_ctx.num_chunks; i++) {
        s_ctx.chunks[i].file_offset = -1;
        s_ctx.chunks[i].valid = false;
        // Clear any stale semaphore signals
        while (xSemaphoreTake(s_ctx.chunks[i].ready, 0) == pdTRUE) {
            // Drain
        }
    }

    // Start reader task if not already running
    if (!s_ctx.reader_task) {
        s_ctx.running = true;
        if (xTaskCreatePinnedToCore(
                sd_reader_task,
                "sd_reader",
                4096,
                NULL,
                6,  // Priority 6
                &s_ctx.reader_task,
                0) != pdPASS) {  // Core 0
            ESP_LOGE(TAG, "Failed to create reader task");
            s_ctx.running = false;
            fclose(s_ctx.file);
            s_ctx.file = NULL;
            xSemaphoreGive(s_ctx.ring_mutex);
            return ESP_ERR_NO_MEM;
        }
    } else {
        s_ctx.running = true;
    }

    xSemaphoreGive(s_ctx.ring_mutex);

    ESP_LOGI(TAG, "Opened file: %s (%lld bytes)", path, (long long)s_ctx.file_size);
    return ESP_OK;
}

ssize_t sd_ring_read_at(off_t offset, void* buf, size_t len)
{
    if (!s_ctx.initialized || !s_ctx.file) {
        return -1;
    }

    if (offset < 0 || offset >= s_ctx.file_size) {
        return -1;
    }

    if (len == 0) {
        return 0;
    }

    uint8_t* dst = (uint8_t*)buf;
    size_t total_read = 0;
    off_t current_offset = offset;

    while (total_read < len && current_offset < s_ctx.file_size) {
        size_t to_read = len - total_read;
        if ((off_t)(current_offset + to_read) > s_ctx.file_size) {
            to_read = (size_t)(s_ctx.file_size - current_offset);
        }

        // Find which chunk contains this offset
        bool found = false;
        size_t chunk_idx = 0;
        
        if (xSemaphoreTake(s_ctx.ring_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "Timeout acquiring ring mutex");
            break;
        }

        // Search for chunk containing this offset
        for (size_t i = 0; i < s_ctx.num_chunks; i++) {
            size_t idx = (s_ctx.read_chunk_idx + i) % s_ctx.num_chunks;
            ring_chunk_t* chunk = &s_ctx.chunks[idx];
            
            if (chunk->valid && chunk->file_offset >= 0) {
                off_t chunk_end = chunk->file_offset + (off_t)chunk->size;
                if (current_offset >= chunk->file_offset && current_offset < chunk_end) {
                    chunk_idx = idx;
                    found = true;
                    break;
                }
            }
        }

        if (!found) {
            // Need to wait for prefetch - update write offset to trigger prefetch
            s_ctx.write_offset = current_offset;
            xSemaphoreGive(s_ctx.ring_mutex);
            
            // Wait a bit for prefetch to catch up
            vTaskDelay(pdMS_TO_TICKS(10));
            
            // Try direct read as fallback
            if (s_ctx.file) {
                fseek(s_ctx.file, current_offset, SEEK_SET);
                size_t direct_read = fread(dst + total_read, 1, to_read, s_ctx.file);
                if (direct_read > 0) {
                    total_read += direct_read;
                    current_offset += (off_t)direct_read;
                }
            }
            continue;
        }

        ring_chunk_t* chunk = &s_ctx.chunks[chunk_idx];
        off_t offset_in_chunk = current_offset - chunk->file_offset;
        size_t available_in_chunk = chunk->size - (size_t)offset_in_chunk;
        size_t read_from_chunk = (to_read < available_in_chunk) ? to_read : available_in_chunk;

        // Wait for chunk to be ready if needed
        xSemaphoreGive(s_ctx.ring_mutex);
        if (xSemaphoreTake(chunk->ready, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "Timeout waiting for chunk ready");
            break;
        }

        // Copy data from chunk
        uint8_t* src = chunk->data + (size_t)offset_in_chunk;
        
        // PSRAM cache sync handled automatically
        memcpy(dst + total_read, src, read_from_chunk);
        
        total_read += read_from_chunk;
        current_offset += (off_t)read_from_chunk;
    }

    return (ssize_t)total_read;
}

ssize_t sd_ring_get_file_size(void)
{
    if (!s_ctx.initialized || !s_ctx.file) {
        return -1;
    }
    return (ssize_t)s_ctx.file_size;
}

void sd_ring_close(void)
{
    if (!s_ctx.initialized) {
        return;
    }

    if (xSemaphoreTake(s_ctx.ring_mutex, portMAX_DELAY) == pdTRUE) {
        s_ctx.running = false;
        xSemaphoreGive(s_ctx.ring_mutex);
    }

    // Wait for reader task to finish
    if (s_ctx.reader_task) {
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
        while (s_ctx.reader_task && xTaskGetTickCount() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s_ctx.reader_task) {
            ESP_LOGW(TAG, "Reader task did not terminate, deleting");
            vTaskDelete(s_ctx.reader_task);
        }
        s_ctx.reader_task = NULL;
    }

    if (xSemaphoreTake(s_ctx.ring_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_ctx.file) {
            fclose(s_ctx.file);
            s_ctx.file = NULL;
        }
        s_ctx.file_path = NULL;
        s_ctx.file_size = -1;
        xSemaphoreGive(s_ctx.ring_mutex);
    }

    ESP_LOGD(TAG, "Closed file");
}

void sd_ring_deinit(void)
{
    sd_ring_close();

    if (!s_ctx.initialized) {
        return;
    }

    if (s_ctx.chunks) {
        for (size_t i = 0; i < s_ctx.num_chunks; i++) {
            if (s_ctx.chunks[i].data) {
                free(s_ctx.chunks[i].data);
            }
            if (s_ctx.chunks[i].ready) {
                vSemaphoreDelete(s_ctx.chunks[i].ready);
            }
        }
        free(s_ctx.chunks);
        s_ctx.chunks = NULL;
    }

    if (s_ctx.ring_mutex) {
        vSemaphoreDelete(s_ctx.ring_mutex);
        s_ctx.ring_mutex = NULL;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    ESP_LOGI(TAG, "SD ring buffer deinitialized");
}

static void sd_reader_task(void* arg)
{
    (void)arg;
    ESP_LOGI(TAG, "SD reader task started");

    while (s_ctx.running) {
        if (!s_ctx.file) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (xSemaphoreTake(s_ctx.ring_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        // Find next chunk to fill
        size_t next_chunk_idx = s_ctx.write_chunk_idx;
        ring_chunk_t* chunk = &s_ctx.chunks[next_chunk_idx];
        
        // Check if we need to prefetch
        off_t target_offset = s_ctx.write_offset;
        if (target_offset >= s_ctx.file_size) {
            xSemaphoreGive(s_ctx.ring_mutex);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Check if chunk already contains this offset
        if (chunk->valid && chunk->file_offset >= 0) {
            off_t chunk_end = chunk->file_offset + (off_t)chunk->size;
            if (target_offset >= chunk->file_offset && target_offset < chunk_end) {
                // Already have this data, advance write offset
                s_ctx.write_offset += (off_t)chunk->size;
                xSemaphoreGive(s_ctx.ring_mutex);
                continue;
            }
        }

        // Fill this chunk
        chunk->file_offset = target_offset;
        chunk->valid = false;
        
        // Clear ready semaphore
        while (xSemaphoreTake(chunk->ready, 0) == pdTRUE) {
            // Drain
        }

        xSemaphoreGive(s_ctx.ring_mutex);

        // Read from file
        fseek(s_ctx.file, target_offset, SEEK_SET);
        size_t read = fread(chunk->data, 1, chunk->size, s_ctx.file);
        
        if (read > 0) {
            // PSRAM write-back is handled automatically by ESP-IDF cache system
            chunk->valid = true;
            s_ctx.write_offset = target_offset + (off_t)read;
            
            // Advance to next chunk
            if (xSemaphoreTake(s_ctx.ring_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                s_ctx.write_chunk_idx = (s_ctx.write_chunk_idx + 1) % s_ctx.num_chunks;
                xSemaphoreGive(s_ctx.ring_mutex);
            }
            
            // Signal chunk ready
            xSemaphoreGive(chunk->ready);
        } else {
            // EOF or error
            if (target_offset >= s_ctx.file_size) {
                // Reached end of file
                vTaskDelay(pdMS_TO_TICKS(10));
            } else {
                ESP_LOGW(TAG, "Read error at offset %lld", (long long)target_offset);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
    }

    ESP_LOGI(TAG, "SD reader task exiting");
    vTaskDelete(NULL);
}

