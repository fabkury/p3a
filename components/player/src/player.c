#include "player.h"
#include "player_internal.h"
#include "player_decoder.h"
#include "player_renderer.h"

#include "sd_ring.h"
#include "scaler_nn.h"
#include "graphics_handoff.h"
#include "gif_decoder.h"
#include "webp/demux.h"
#include "webp/decode.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "player";

#define CACHE_LINE_SIZE 64
#define MAX_NATIVE_SIZE 128
#define NATIVE_BUFFER_SIZE (MAX_NATIVE_SIZE * MAX_NATIVE_SIZE * 3)  // RGB888
#define HSTRIP 16
#define STRIP_SIZE (720 * HSTRIP * 3)  // RGB888

// Forward declarations
static esp_err_t init_buffers(void);
static void cleanup_buffers(void);
static void start_renderer(void);
static void stop_renderer(void);

static player_ctx_t s_player = {0};

esp_err_t player_init(void)
{
    ESP_LOGI(TAG, "=== Player init start ===");
    
    if (s_player.native_buf1) {
        ESP_LOGW(TAG, "Player already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing player system");

    // Initialize scaler maps
    ESP_LOGI(TAG, "Initializing scaler maps...");
    nn_init_all_maps();
    ESP_LOGI(TAG, "Scaler maps initialized");

    // Initialize buffers
    ESP_LOGI(TAG, "Initializing buffers...");
    esp_err_t ret = init_buffers();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Buffer initialization failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Buffers initialized successfully");

    // Create queues
    ESP_LOGI(TAG, "Creating queues...");
    s_player.dec2ren_q = xQueueCreate(2, sizeof(uint8_t*));  // Frame ready notification
    s_player.ren2dec_q = xQueueCreate(2, sizeof(uint8_t*));  // Back-pressure
    
    if (!s_player.dec2ren_q || !s_player.ren2dec_q) {
        ESP_LOGE(TAG, "Failed to create queues (dec2ren=%p, ren2dec=%p)", 
                 (void*)s_player.dec2ren_q, (void*)s_player.ren2dec_q);
        cleanup_buffers();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Queues created successfully");

    s_player.running = false;
    s_player.native_width = 0;
    s_player.native_height = 0;
    s_player.panel = NULL;
    s_player.trans_sem = NULL;
    s_player.scaler_map = NULL;

    ESP_LOGI(TAG, "=== Player init complete ===");
    return ESP_OK;
}

static esp_err_t init_buffers(void)
{
    ESP_LOGI(TAG, "Allocating native buffers (PSRAM, %zu bytes each)...", NATIVE_BUFFER_SIZE);
    
    // Allocate native buffers in PSRAM (double-buffered)
    s_player.native_buf1 = (uint8_t*)heap_caps_aligned_alloc(
        CACHE_LINE_SIZE,
        NATIVE_BUFFER_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    
    s_player.native_buf2 = (uint8_t*)heap_caps_aligned_alloc(
        CACHE_LINE_SIZE,
        NATIVE_BUFFER_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!s_player.native_buf1 || !s_player.native_buf2) {
        ESP_LOGE(TAG, "Failed to allocate native buffers (buf1=%p, buf2=%p)", 
                 (void*)s_player.native_buf1, (void*)s_player.native_buf2);
        cleanup_buffers();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Native buffers allocated: buf1=%p, buf2=%p", 
             (void*)s_player.native_buf1, (void*)s_player.native_buf2);

    ESP_LOGI(TAG, "Allocating strip buffers (SRAM, %zu bytes each)...", STRIP_SIZE);
    
    // Allocate strip buffers in SRAM (ping-pong, DMA-capable)
    s_player.strip_buf1 = (uint8_t*)heap_caps_aligned_alloc(
        CACHE_LINE_SIZE,
        STRIP_SIZE,
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    
    s_player.strip_buf2 = (uint8_t*)heap_caps_aligned_alloc(
        CACHE_LINE_SIZE,
        STRIP_SIZE,
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (!s_player.strip_buf1 || !s_player.strip_buf2) {
        ESP_LOGE(TAG, "Failed to allocate strip buffers (buf1=%p, buf2=%p)", 
                 (void*)s_player.strip_buf1, (void*)s_player.strip_buf2);
        cleanup_buffers();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Strip buffers allocated: buf1=%p, buf2=%p", 
             (void*)s_player.strip_buf1, (void*)s_player.strip_buf2);
    
    // Verify buffers are in SRAM (not PSRAM) - SRAM addresses are typically 0x4FFxxxxx or 0x400xxxxx
    // PSRAM addresses are typically 0x48xxxxxx or 0x50xxxxxx
    uintptr_t strip1_addr = (uintptr_t)s_player.strip_buf1;
    uintptr_t strip2_addr = (uintptr_t)s_player.strip_buf2;
    bool strip1_sram = (strip1_addr >= 0x40000000 && strip1_addr < 0x50000000);
    bool strip2_sram = (strip2_addr >= 0x40000000 && strip2_addr < 0x50000000);
    ESP_LOGI(TAG, "Strip buffer verification: buf1=0x%08x (SRAM=%d), buf2=0x%08x (SRAM=%d)",
             (unsigned int)strip1_addr, strip1_sram ? 1 : 0,
             (unsigned int)strip2_addr, strip2_sram ? 1 : 0);
    
    if (!strip1_sram || !strip2_sram) {
        ESP_LOGW(TAG, "WARNING: Strip buffers may not be in SRAM! This could cause DMA underruns.");
    }

    s_player.nwrite = s_player.native_buf1;
    s_player.nread = s_player.native_buf2;
    s_player.strip_ping = s_player.strip_buf1;
    s_player.strip_pong = s_player.strip_buf2;

    ESP_LOGI(TAG, "All buffers allocated successfully: native=%zu KiB each, strip=%zu KiB each",
             NATIVE_BUFFER_SIZE / 1024, STRIP_SIZE / 1024);
    
    // Log memory usage
    size_t free_heap = esp_get_free_heap_size();
    size_t min_free_heap = esp_get_minimum_free_heap_size();
    ESP_LOGI(TAG, "Heap after buffer allocation: free=%zu bytes, min_free=%zu bytes", 
             free_heap, min_free_heap);
    
    return ESP_OK;
}

static void cleanup_buffers(void)
{
    if (s_player.native_buf1) {
        free(s_player.native_buf1);
        s_player.native_buf1 = NULL;
    }
    if (s_player.native_buf2) {
        free(s_player.native_buf2);
        s_player.native_buf2 = NULL;
    }
    if (s_player.strip_buf1) {
        free(s_player.strip_buf1);
        s_player.strip_buf1 = NULL;
    }
    if (s_player.strip_buf2) {
        free(s_player.strip_buf2);
        s_player.strip_buf2 = NULL;
    }
}

bool player_start(const anim_desc_t* desc)
{
    ESP_LOGI(TAG, "=== Player start: path='%s', type=%d, size=%d ===", 
             desc ? desc->path : "NULL", desc ? desc->type : -1, 
             desc ? desc->native_size_px : -1);
    
    if (!desc || !desc->path) {
        ESP_LOGE(TAG, "Invalid animation descriptor (desc=%p, path=%p)", 
                 (void*)desc, desc ? desc->path : NULL);
        return false;
    }

    // Validate native size
    if (desc->native_size_px != 16 && desc->native_size_px != 32 &&
        desc->native_size_px != 64 && desc->native_size_px != 128) {
        ESP_LOGE(TAG, "Invalid native size: %d (must be 16, 32, 64, or 128)", desc->native_size_px);
        return false;
    }

    if (s_player.running) {
        ESP_LOGW(TAG, "Player already running, stopping first...");
        player_stop();
    }

    // Get scaler map
    ESP_LOGI(TAG, "Getting scaler map for size %d...", desc->native_size_px);
    s_player.scaler_map = nn_get_map(desc->native_size_px);
    if (!s_player.scaler_map) {
        ESP_LOGE(TAG, "Failed to get scaler map for size %d", desc->native_size_px);
        return false;
    }
    ESP_LOGI(TAG, "Scaler map obtained: %p", (void*)s_player.scaler_map);

    // Copy descriptor
    memcpy(&s_player.current_desc, desc, sizeof(anim_desc_t));
    s_player.native_width = desc->native_size_px;
    s_player.native_height = desc->native_size_px;

    // Enter player mode (acquire panel)
    ESP_LOGI(TAG, "Entering player mode (graphics handoff)...");
    esp_err_t ret = graphics_handoff_enter_player_mode(&s_player.panel, (void**)&s_player.trans_sem);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enter player mode: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "Player mode entered: panel=%p, trans_sem=%p", 
             (void*)s_player.panel, (void*)s_player.trans_sem);

    // Open file in SD ring buffer
    ESP_LOGI(TAG, "Opening file in SD ring: '%s'...", desc->path);
    ret = sd_ring_open_file(desc->path);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open file in SD ring: %s", esp_err_to_name(ret));
        graphics_handoff_enter_lvgl_mode();
        return false;
    }
    ESP_LOGI(TAG, "File opened in SD ring");

    // Set running flag BEFORE starting tasks to avoid race condition
    s_player.running = true;

    // Start decoder
    ESP_LOGI(TAG, "Starting decoder...");
    ret = start_decoder(desc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start decoder: %s", esp_err_to_name(ret));
        s_player.running = false;
        sd_ring_close();
        graphics_handoff_enter_lvgl_mode();
        return false;
    }
    ESP_LOGI(TAG, "Decoder started");
    
    // Update scaler map if decoder detected different size than descriptor
    // Wait a moment for decoder to initialize and detect actual size
    vTaskDelay(pdMS_TO_TICKS(100));
    if (s_player.native_width != desc->native_size_px || s_player.native_height != desc->native_size_px) {
        ESP_LOGW(TAG, "Updating scaler map: descriptor=%dx%d, actual=%dx%d",
                 desc->native_size_px, desc->native_size_px,
                 s_player.native_width, s_player.native_height);
        s_player.scaler_map = nn_get_map(s_player.native_width);
        if (!s_player.scaler_map) {
            ESP_LOGE(TAG, "Failed to get scaler map for actual size %d", s_player.native_width);
            s_player.running = false;
            stop_decoder();
            sd_ring_close();
            graphics_handoff_enter_lvgl_mode();
            return false;
        }
        ESP_LOGI(TAG, "Scaler map updated for actual size: %dx%d", s_player.native_width, s_player.native_height);
    }

    // Start renderer
    ESP_LOGI(TAG, "Starting renderer...");
    start_renderer();
    ESP_LOGI(TAG, "Renderer started");
    ESP_LOGI(TAG, "=== Player started successfully: %s (%dx%d) ===", 
             desc->path, desc->native_size_px, desc->native_size_px);
    return true;
}

void player_stop(void)
{
    if (!s_player.running) {
        return;
    }

    ESP_LOGI(TAG, "Stopping player");

    s_player.running = false;

    // Stop decoder and renderer tasks
    stop_renderer();
    stop_decoder();

    // Close SD ring
    sd_ring_close();

    // Exit player mode (release panel)
    graphics_handoff_enter_lvgl_mode();

    s_player.panel = NULL;
    s_player.trans_sem = NULL;
    s_player.scaler_map = NULL;

    ESP_LOGI(TAG, "Player stopped");
}

bool player_is_running(void)
{
    return s_player.running;
}

// Internal functions for decoder/renderer
player_ctx_t* player_get_ctx(void)
{
    return &s_player;
}

// Forward declarations for renderer functions
extern void player_renderer_start(void);
extern void player_renderer_stop(void);

static void start_renderer(void)
{
    player_renderer_start();
}

static void stop_renderer(void)
{
    player_renderer_stop();
}

