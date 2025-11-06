#include "renderer.h"

#include <string.h>
#include <strings.h>  // For strcasecmp
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "webp/demux.h"
#include "webp/decode.h"
#include "storage/fs.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_4b.h"
#include "lvgl.h"
#include "driver/ppa.h"
#include "soc/soc_caps.h"
#include "video_player.h"

static const char *TAG = "renderer";

// Playback policy toggles
static const bool s_renderer_include_gif = true;
static const bool s_renderer_include_webp = true;

#define MAX_ANIMATION_FILES 32
#define ANIMATION_DIR "/sdcard/animations"
#define FALLBACK_DIR "/sdcard"

typedef struct {
    char path[256];
    char name[64];
} animation_file_t;

typedef struct {
    animation_file_t files[MAX_ANIMATION_FILES];
    size_t count;
    size_t current_index;
    bool initialized;
    
    // Current animation state
    WebPAnimDecoder *decoder;
    WebPAnimInfo anim_info;
    uint8_t *webp_data;
    size_t webp_size;
    int *frame_delays;
    size_t frame_count;
    size_t frame_index;
    int64_t last_frame_time_us;
    int current_frame_delay_ms;
    
    // LVGL widgets
    lv_obj_t *canvas;
    uint8_t *canvas_buffer;
    size_t canvas_buffer_size;
    bool canvas_buffer_spiram;  // True if buffer is in SPIRAM
    
    // FPS tracking (for status only, not displayed)
    float current_fps;
    
    // Performance profiling
    int64_t decode_time_us;
    int64_t blit_time_us;
    int64_t flush_time_us;
    int64_t frame_interval_us;
    uint32_t frame_count_profile;
    int64_t last_profile_log_us;
    
    // Precomputed scaling indices for performance optimization
    int *x_index_map;      // Precomputed X source indices for each destination X
    int *y_index_map;      // Precomputed Y source indices for each destination Y
    size_t x_index_map_size;
    size_t y_index_map_size;
    
    // PPA (Pixel Processing Accelerator) for hardware-accelerated scaling
    ppa_client_handle_t ppa_srm_handle;  // PPA SRM client handle
    uint8_t *source_rgb888_buffer;        // Temporary RGB888 buffer for PPA input
    size_t source_rgb888_buffer_size;     // Size of source RGB888 buffer
    bool ppa_available;                    // True if PPA is available and initialized
    bool using_ppa;                        // True if currently using PPA for scaling
    
    // Thread safety
    SemaphoreHandle_t mutex;
    bool pending_cycle;
    int64_t last_cycle_time_us;  // Timestamp of last cycle to prevent rapid-fire cycles
    
    // Video player mode
    bool use_video_player;  // Use bypass mode for animations
} renderer_state_t;

static renderer_state_t s_renderer = {0};

static inline void copy_rgba_to_rgb888(uint8_t *dst, const uint8_t *src)
{
    // WebP outputs RGBA, but display expects BGR888 (swap R and B)
    dst[0] = src[2];  // B
    dst[1] = src[1];  // G
    dst[2] = src[0];  // R
}

static void blit_rgba_to_rgb888(const uint8_t *src_rgba, int src_w, int src_h,
                                uint8_t *dst_rgb, int dst_w, int dst_h)
{
    if (!src_rgba || !dst_rgb || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return;
    }

    const size_t src_stride = (size_t)src_w * 4;
    const size_t dst_stride = (size_t)dst_w * 3;

    // Fast path: 1:1 copy (no scaling)
    if (src_w == dst_w && src_h == dst_h) {
        for (int y = 0; y < src_h; ++y) {
            const uint8_t *src_row = src_rgba + (size_t)y * src_stride;
            uint8_t *dst_row = dst_rgb + (size_t)y * dst_stride;
            for (int x = 0; x < src_w; ++x) {
                const uint8_t *src_px = src_row + (size_t)x * 4;
                uint8_t *dst_px = dst_row + (size_t)x * 3;
                copy_rgba_to_rgb888(dst_px, src_px);
            }
        }
        return;
    }

    // Optimized scaling with precomputed indices
    if (s_renderer.x_index_map && s_renderer.y_index_map &&
        s_renderer.x_index_map_size == (size_t)dst_w &&
        s_renderer.y_index_map_size == (size_t)dst_h) {

        for (int y = 0; y < dst_h; ++y) {
            const int src_y = s_renderer.y_index_map[y];
            if (src_y >= src_h) {
                continue;
            }

            const uint8_t *src_row = src_rgba + (size_t)src_y * src_stride;
            uint8_t *dst_row = dst_rgb + (size_t)y * dst_stride;

            for (int x = 0; x < dst_w; ++x) {
                int src_x = s_renderer.x_index_map[x];
                const uint8_t *src_px = src_row + (size_t)src_x * 4;
                uint8_t *dst_px = dst_row + (size_t)x * 3;
                copy_rgba_to_rgb888(dst_px, src_px);
            }
        }
        return;
    }

    // Fallback: compute indices on-the-fly (slower, but works if precomputed maps not available)
    const uint32_t y_step = (src_h << 16) / dst_h;
    const uint32_t x_step = (src_w << 16) / dst_w;

    uint32_t src_y_acc = 0;
    for (int y = 0; y < dst_h; ++y) {
        int src_y = (int)(src_y_acc >> 16);
        if (src_y >= src_h) {
            src_y = src_h - 1;
        }

        const uint8_t *src_row = src_rgba + (size_t)src_y * src_stride;
        uint8_t *dst_row = dst_rgb + (size_t)y * dst_stride;

        uint32_t src_x_acc = 0;
        for (int x = 0; x < dst_w; ++x) {
            int src_x = (int)(src_x_acc >> 16);
            if (src_x >= src_w) {
                src_x = src_w - 1;
            }

            const uint8_t *src_px = src_row + (size_t)src_x * 4;
            uint8_t *dst_px = dst_row + (size_t)x * 3;
            copy_rgba_to_rgb888(dst_px, src_px);

            src_x_acc += x_step;
        }

        src_y_acc += y_step;
    }
}

static esp_err_t scan_animation_directory(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir) {
        return ESP_ERR_NOT_FOUND;
    }

    s_renderer.count = 0;
    struct dirent *entry;
    
    while ((entry = readdir(dir)) != NULL && s_renderer.count < MAX_ANIMATION_FILES) {
        const char *name = entry->d_name;
        size_t len = strlen(name);
        
        bool is_webp = (len > 5 && strcmp(name + len - 5, ".webp") == 0);
        bool is_gif = (len > 4 && strcmp(name + len - 4, ".gif") == 0);

        if (is_webp && !s_renderer_include_webp) {
            continue;
        }
        if (is_gif && !s_renderer_include_gif) {
            continue;
        }

        if (is_webp) {
            animation_file_t *file = &s_renderer.files[s_renderer.count];
            int ret = snprintf(file->path, sizeof(file->path), "%s/%s", dir_path, name);
            if (ret < 0 || ret >= (int)sizeof(file->path)) {
                ESP_LOGW(TAG, "Path too long, skipping: %s/%s", dir_path, name);
                continue;
            }
            strncpy(file->name, name, sizeof(file->name) - 1);
            file->name[sizeof(file->name) - 1] = '\0';
            s_renderer.count++;
            ESP_LOGI(TAG, "Found animation: %s", file->path);
        } else if (is_gif) {
            animation_file_t *file = &s_renderer.files[s_renderer.count];
            int ret = snprintf(file->path, sizeof(file->path), "%s/%s", dir_path, name);
            if (ret < 0 || ret >= (int)sizeof(file->path)) {
                ESP_LOGW(TAG, "Path too long, skipping: %s/%s", dir_path, name);
                continue;
            }
            strncpy(file->name, name, sizeof(file->name) - 1);
            file->name[sizeof(file->name) - 1] = '\0';
            s_renderer.count++;
            ESP_LOGI(TAG, "Found animation: %s", file->path);
        }
    }
    
    closedir(dir);
    return ESP_OK;
}

static void free_index_maps(void)
{
    if (s_renderer.x_index_map) {
        free(s_renderer.x_index_map);
        s_renderer.x_index_map = NULL;
    }
    if (s_renderer.y_index_map) {
        free(s_renderer.y_index_map);
        s_renderer.y_index_map = NULL;
    }
    s_renderer.x_index_map_size = 0;
    s_renderer.y_index_map_size = 0;
}

static esp_err_t compute_index_maps(int src_w, int src_h, int dst_w, int dst_h)
{
    // Free existing maps if any
    free_index_maps();
    
    // Allocate index maps
    s_renderer.x_index_map = (int *)malloc((size_t)dst_w * sizeof(int));
    s_renderer.y_index_map = (int *)malloc((size_t)dst_h * sizeof(int));
    
    if (!s_renderer.x_index_map || !s_renderer.y_index_map) {
        free_index_maps();
        return ESP_ERR_NO_MEM;
    }
    
    s_renderer.x_index_map_size = (size_t)dst_w;
    s_renderer.y_index_map_size = (size_t)dst_h;
    
    // Precompute X indices
    if (dst_w > 1) {
        const uint32_t x_step = (src_w << 16) / dst_w;
        uint32_t src_x_acc = 0;
        for (int x = 0; x < dst_w; ++x) {
            int src_x = (src_x_acc >> 16);
            if (src_x >= src_w) src_x = src_w - 1;
            s_renderer.x_index_map[x] = src_x;
            src_x_acc += x_step;
        }
    } else {
        s_renderer.x_index_map[0] = 0;
    }
    
    // Precompute Y indices
    if (dst_h > 1) {
        const uint32_t y_step = (src_h << 16) / dst_h;
        uint32_t src_y_acc = 0;
        for (int y = 0; y < dst_h; ++y) {
            int src_y = (src_y_acc >> 16);
            if (src_y >= src_h) src_y = src_h - 1;
            s_renderer.y_index_map[y] = src_y;
            src_y_acc += y_step;
        }
    } else {
        s_renderer.y_index_map[0] = 0;
    }
    
    return ESP_OK;
}

static void unload_current_animation(void)
{
    if (s_renderer.decoder) {
        WebPAnimDecoderDelete(s_renderer.decoder);
        s_renderer.decoder = NULL;
    }
    
    free(s_renderer.frame_delays);
    s_renderer.frame_delays = NULL;
    
    free(s_renderer.webp_data);
    s_renderer.webp_data = NULL;
    
    free_index_maps();
    
    s_renderer.webp_size = 0;
    s_renderer.frame_count = 0;
    s_renderer.frame_index = 0;
    s_renderer.current_frame_delay_ms = 16;
    memset(&s_renderer.anim_info, 0, sizeof(s_renderer.anim_info));
}

static esp_err_t init_ppa(void)
{
#if SOC_PPA_SUPPORTED
    if (s_renderer.ppa_available) {
        return ESP_OK;  // Already initialized
    }
    
    ESP_LOGI(TAG, "[PPA] Initializing Pixel Processing Accelerator...");
    
    // Register PPA SRM client
    ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
    };
    
    esp_err_t ret = ppa_register_client(&ppa_config, &s_renderer.ppa_srm_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "[PPA] Failed to register PPA SRM client: %s", esp_err_to_name(ret));
        s_renderer.ppa_available = false;
        return ret;
    }
    
    s_renderer.ppa_available = true;
    s_renderer.using_ppa = false;
    ESP_LOGI(TAG, "[PPA] PPA SRM client registered successfully");
    ESP_LOGI(TAG, "[PPA] Hardware acceleration available for scaling");
    return ESP_OK;
#else
    ESP_LOGW(TAG, "[PPA] PPA not supported on this chip");
    s_renderer.ppa_available = false;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static void cleanup_ppa(void) __attribute__((unused));
static void cleanup_ppa(void)
{
#if SOC_PPA_SUPPORTED
    if (s_renderer.ppa_srm_handle) {
        ppa_unregister_client(s_renderer.ppa_srm_handle);
        s_renderer.ppa_srm_handle = NULL;
    }
    
    if (s_renderer.source_rgb888_buffer) {
        free(s_renderer.source_rgb888_buffer);
        s_renderer.source_rgb888_buffer = NULL;
    }
    
    s_renderer.ppa_available = false;
    s_renderer.using_ppa = false;
    s_renderer.source_rgb888_buffer_size = 0;
#endif
}

static esp_err_t ensure_source_rgb888_buffer(int src_w, int src_h)
{
    size_t required_size = (size_t)src_w * src_h * 3;  // RGB888 = 3 bytes per pixel
    
    if (s_renderer.source_rgb888_buffer_size >= required_size) {
        return ESP_OK;  // Buffer already exists and is large enough
    }
    
    // Free old buffer if exists
    if (s_renderer.source_rgb888_buffer) {
        free(s_renderer.source_rgb888_buffer);
        s_renderer.source_rgb888_buffer = NULL;
    }
    
    // Allocate new buffer - try DMA-capable internal memory first
    const size_t cache_line_size = 64;
    size_t aligned_size = ((required_size + cache_line_size - 1) / cache_line_size) * cache_line_size;
    
    s_renderer.source_rgb888_buffer = (uint8_t *)heap_caps_aligned_alloc(
        cache_line_size,
        aligned_size,
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    
    if (!s_renderer.source_rgb888_buffer) {
        ESP_LOGW(TAG, "[PPA] DMA allocation failed for source buffer, trying regular internal memory");
        s_renderer.source_rgb888_buffer = (uint8_t *)heap_caps_aligned_alloc(
            cache_line_size,
            aligned_size,
            MALLOC_CAP_INTERNAL);
    }
    
    if (!s_renderer.source_rgb888_buffer) {
        ESP_LOGW(TAG, "[PPA] Internal memory allocation failed, trying SPIRAM");
        s_renderer.source_rgb888_buffer = (uint8_t *)heap_caps_aligned_alloc(
            cache_line_size,
            aligned_size,
            MALLOC_CAP_SPIRAM);
    }
    
    if (!s_renderer.source_rgb888_buffer) {
        ESP_LOGE(TAG, "[PPA] Failed to allocate source RGB888 buffer (%zu bytes)", aligned_size);
        return ESP_ERR_NO_MEM;
    }
    
    s_renderer.source_rgb888_buffer_size = aligned_size;
    
    bool is_dma = ((uintptr_t)s_renderer.source_rgb888_buffer >= 0x3FC00000 && 
                   (uintptr_t)s_renderer.source_rgb888_buffer < 0x40000000);
    ESP_LOGI(TAG, "[PPA] Allocated source RGB888 buffer: %zu bytes, %s", 
             aligned_size, is_dma ? "DMA-capable" : "SPIRAM");
    
    return ESP_OK;
}

// Copy RGB data into an aligned RGB888 buffer (no scaling) - optimized for PPA input
static void copy_rgba_to_rgb888_fast(const uint8_t *src_rgba, int src_w, int src_h,
                                     uint8_t *dst_rgb)
{
    const size_t src_row_bytes = (size_t)src_w * 4;
    const size_t dst_row_bytes = (size_t)src_w * 3;
    for (int y = 0; y < src_h; ++y) {
        const uint8_t *src_row = src_rgba + (size_t)y * src_row_bytes;
        uint8_t *dst_row = dst_rgb + (size_t)y * dst_row_bytes;
        for (int x = 0; x < src_w; ++x) {
            const uint8_t *src_px = src_row + (size_t)x * 4;
            uint8_t *dst_px = dst_row + (size_t)x * 3;
            copy_rgba_to_rgb888(dst_px, src_px);
        }
    }
}

// Use PPA for hardware-accelerated scaling (RGB888 -> RGB888)
static esp_err_t blit_rgb888_with_ppa(const uint8_t *src_rgb888, int src_w, int src_h,
                                      uint8_t *dst_rgb888, int dst_w, int dst_h)
{
#if SOC_PPA_SUPPORTED
    if (!s_renderer.ppa_available || !s_renderer.ppa_srm_handle) {
        ESP_LOGW(TAG, "[PPA] PPA not available, falling back to software scaling");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Calculate scale factors
    float scale_x = (float)dst_w / (float)src_w;
    float scale_y = (float)dst_h / (float)src_h;
    
    // Configure PPA SRM operation - use nearest neighbor mode (no interpolation)
    ppa_srm_oper_config_t srm_config = {
        .in.buffer = (uint8_t *)src_rgb888,
        .in.pic_w = src_w,
        .in.pic_h = src_h,
        .in.block_w = src_w,
        .in.block_h = src_h,
        .in.block_offset_x = 0,
        .in.block_offset_y = 0,
        .in.srm_cm = PPA_SRM_COLOR_MODE_RGB888,
        
        .out.buffer = (uint8_t *)dst_rgb888,
        .out.buffer_size = (size_t)dst_w * dst_h * 3,
        .out.pic_w = dst_w,
        .out.pic_h = dst_h,
        .out.block_offset_x = 0,
        .out.block_offset_y = 0,
        .out.srm_cm = PPA_SRM_COLOR_MODE_RGB888,
        
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = scale_x,
        .scale_y = scale_y,
        
        .byte_swap = false,
        
        // Use nearest neighbor mode (no interpolation) for crisp, pixelated scaling
        // According to ESP-IDF PPA documentation:
        // Mode 0 = nearest neighbor (point sampling) - crisp, pixelated
        // Mode 1 = bilinear interpolation - smooth but slower
        // We want nearest neighbor for pixel-perfect upscaling
        .mode = 0,  // Nearest neighbor mode (explicitly set to 0)
        
        .user_data = NULL,
    };
    
    // Execute PPA SRM operation (synchronous)
    esp_err_t ret = ppa_do_scale_rotate_mirror(s_renderer.ppa_srm_handle, &srm_config);
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "[PPA] PPA scaling failed: %s, falling back to software", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGD(TAG, "[PPA] Hardware scaling completed: %dx%d -> %dx%d (scale: %.2fx%.2f)", 
             src_w, src_h, dst_w, dst_h, scale_x, scale_y);
    
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t load_animation_file(const char *file_path)
{
    FILE *f = fopen(file_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", file_path);
        return ESP_ERR_NOT_FOUND;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (file_size <= 0 || file_size > 10 * 1024 * 1024) {  // Max 10MB
        fclose(f);
        ESP_LOGE(TAG, "Invalid file size: %ld", file_size);
        return ESP_ERR_INVALID_SIZE;
    }

    // Allocate and read file
    uint8_t *data = (uint8_t *)malloc((size_t)file_size);
    if (!data) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t read = fread(data, 1, (size_t)file_size, f);
    fclose(f);

    if (read != (size_t)file_size) {
        free(data);
        ESP_LOGE(TAG, "Failed to read entire file");
        return ESP_FAIL;
    }

    // Unload previous animation
    unload_current_animation();

    // Initialize WebP decoder
    WebPAnimDecoderOptions dec_opts;
    if (!WebPAnimDecoderOptionsInit(&dec_opts)) {
        free(data);
        return ESP_FAIL;
    }
    dec_opts.color_mode = MODE_RGBA;
    dec_opts.use_threads = 0;

    WebPData webp_data = {
        .bytes = data,
        .size = (size_t)file_size,
    };

    s_renderer.decoder = WebPAnimDecoderNew(&webp_data, &dec_opts);
    if (!s_renderer.decoder) {
        free(data);
        ESP_LOGE(TAG, "Failed to create WebP decoder");
        return ESP_FAIL;
    }

    if (!WebPAnimDecoderGetInfo(s_renderer.decoder, &s_renderer.anim_info)) {
        WebPAnimDecoderDelete(s_renderer.decoder);
        s_renderer.decoder = NULL;
        free(data);
        ESP_LOGE(TAG, "Failed to get WebP animation info");
        return ESP_FAIL;
    }

    s_renderer.frame_count = s_renderer.anim_info.frame_count;
    if (s_renderer.frame_count == 0 || 
        s_renderer.anim_info.canvas_width == 0 || 
        s_renderer.anim_info.canvas_height == 0) {
        unload_current_animation();
        free(data);
        ESP_LOGE(TAG, "Invalid WebP animation");
        return ESP_FAIL;
    }

    // Extract frame delays
    const WebPDemuxer *demux = WebPAnimDecoderGetDemuxer(s_renderer.decoder);
    if (!demux) {
        unload_current_animation();
        free(data);
        return ESP_FAIL;
    }

    s_renderer.frame_delays = (int *)calloc(s_renderer.frame_count, sizeof(int));
    if (!s_renderer.frame_delays) {
        unload_current_animation();
        free(data);
        return ESP_ERR_NO_MEM;
    }

    WebPIterator iter;
    if (WebPDemuxGetFrame(demux, 1, &iter)) {
        do {
            size_t idx = (size_t)iter.frame_num - 1;
            if (idx < s_renderer.frame_count) {
                s_renderer.frame_delays[idx] = iter.duration;
            }
        } while (WebPDemuxNextFrame(&iter));
        WebPDemuxReleaseIterator(&iter);
    }

    // Set fallback delays
    for (size_t i = 0; i < s_renderer.frame_count; ++i) {
        if (s_renderer.frame_delays[i] <= 0) {
            s_renderer.frame_delays[i] = 16;
        }
    }

    s_renderer.webp_data = data;
    s_renderer.webp_size = (size_t)file_size;
    s_renderer.frame_index = 0;
    s_renderer.last_frame_time_us = esp_timer_get_time();
    
    if (s_renderer.frame_count > 0) {
        s_renderer.current_frame_delay_ms = s_renderer.frame_delays[0];
    }
    
    // If video player is available, use it for playback (bypass LVGL)
    if (s_renderer.use_video_player) {
        ESP_LOGI(TAG, "[RENDERER] load_animation_file() - Starting video player (is_playing=%d, use_video_player=%d)", 
                 video_player_is_playing(), s_renderer.use_video_player);
            // Stop any existing playback first (but keep bypass mode for seamless switch)
            if (video_player_is_playing()) {
                ESP_LOGI(TAG, "[RENDERER] Stopping existing video player before starting new one");
                video_player_stop(true);  // Keep bypass mode active
                vTaskDelay(pdMS_TO_TICKS(200));  // Wait for stop to complete
                ESP_LOGI(TAG, "[RENDERER] After stop: video_player_is_playing=%d", video_player_is_playing());
            }
        ESP_LOGI(TAG, "[RENDERER] Calling video_player_play_webp() with file_size=%zu", (size_t)file_size);
        esp_err_t vp_ret = video_player_play_webp(data, (size_t)file_size, true);
        ESP_LOGI(TAG, "[RENDERER] video_player_play_webp() returned: %s (ESP_OK=%d)", 
                 esp_err_to_name(vp_ret), (vp_ret == ESP_OK));
        if (vp_ret == ESP_OK) {
            ESP_LOGI(TAG, "[RENDERER] Video player started successfully - video_player_is_playing=%d", video_player_is_playing());
            // Keep decoder state for status queries, but playback happens in video player
            return ESP_OK;
        } else {
            ESP_LOGW(TAG, "[RENDERER] Video player failed, falling back to LVGL mode: %s", esp_err_to_name(vp_ret));
            s_renderer.use_video_player = false;  // Disable for this session
        }
    } else {
        ESP_LOGI(TAG, "[RENDERER] load_animation_file() - use_video_player=%d, will use LVGL mode", s_renderer.use_video_player);
    }

    // Precompute scaling indices for optimized blitting
    // Get canvas size for index map computation
    lv_coord_t canvas_w = 720;
    lv_coord_t canvas_h = 720;
    if (s_renderer.canvas) {
        canvas_w = lv_obj_get_width(s_renderer.canvas);
        canvas_h = lv_obj_get_height(s_renderer.canvas);
    }
    if (canvas_w <= 0) canvas_w = 720;
    if (canvas_h <= 0) canvas_h = 720;
    
    esp_err_t idx_ret = compute_index_maps(
        (int)s_renderer.anim_info.canvas_width,
        (int)s_renderer.anim_info.canvas_height,
        (int)canvas_w,
        (int)canvas_h);
    if (idx_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to compute index maps, will use slower on-the-fly computation");
    } else {
        ESP_LOGI(TAG, "Precomputed scaling indices: %ux%u -> %ux%u",
                 (unsigned)s_renderer.anim_info.canvas_width,
                 (unsigned)s_renderer.anim_info.canvas_height,
                 (unsigned)canvas_w, (unsigned)canvas_h);
    }

    ESP_LOGI(TAG, "Loaded animation: %ux%u, %zu frames",
             (unsigned)s_renderer.anim_info.canvas_width,
             (unsigned)s_renderer.anim_info.canvas_height,
             s_renderer.frame_count);

    return ESP_OK;
}

static esp_err_t create_canvas_buffer(void)
{
    if (s_renderer.canvas_buffer) {
        return ESP_OK;
    }

    // Get canvas size - try canvas first, then display resolution
    lv_coord_t width = 720;
    lv_coord_t height = 720;
    
    if (s_renderer.canvas) {
        width = lv_obj_get_width(s_renderer.canvas);
        height = lv_obj_get_height(s_renderer.canvas);
    }
    
    // If canvas size not available, use display resolution
    if (width <= 0 || height <= 0) {
        lv_display_t *disp = lv_display_get_default();
        if (disp) {
            width = lv_display_get_horizontal_resolution(disp);
            height = lv_display_get_vertical_resolution(disp);
        }
        
        // Fallback to known display size
        if (width <= 0 || height <= 0) {
            width = 720;
            height = 720;
        }
    }

    // RGB888 buffer: 3 bytes per pixel
    // Align to cache line (64 bytes on ESP32-P4) for better DMA performance
    const size_t cache_line_size = 64;  // ESP32-P4 L1 cache line size
    s_renderer.canvas_buffer_size = (size_t)width * height * 3;
    size_t aligned_size = ((s_renderer.canvas_buffer_size + cache_line_size - 1) / cache_line_size) * cache_line_size;
    
    // Aggressively try DMA-capable internal memory first (best performance)
    // Try multiple strategies to get DMA memory
    s_renderer.canvas_buffer = (uint8_t *)heap_caps_aligned_alloc(
        cache_line_size,
        aligned_size,
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    
    // If DMA fails, try smaller chunk allocation strategy
    if (!s_renderer.canvas_buffer) {
        // Try allocating with 8-bit alignment requirement (DMA-capable)
        s_renderer.canvas_buffer = (uint8_t *)heap_caps_aligned_alloc(
            cache_line_size,
            aligned_size,
            MALLOC_CAP_DMA | MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    }
    
    // Fallback to regular aligned internal memory if DMA fails
    if (!s_renderer.canvas_buffer) {
        ESP_LOGW(TAG, "DMA allocation failed, trying regular internal memory");
        s_renderer.canvas_buffer = (uint8_t *)heap_caps_aligned_alloc(
            cache_line_size,
            aligned_size,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    
    // Last resort: try SPIRAM (not ideal for DMA but better than nothing)
    // Note: SPIRAM buffers may not be cache-aligned, so we'll skip cache sync for them
    if (!s_renderer.canvas_buffer) {
        ESP_LOGW(TAG, "Internal memory allocation failed, trying SPIRAM");
        s_renderer.canvas_buffer = (uint8_t *)heap_caps_aligned_alloc(
            cache_line_size,
            aligned_size,
            MALLOC_CAP_SPIRAM);
    }
    
    // Check if buffer is in SPIRAM (addresses in SPIRAM range typically start with 0x48)
    bool is_spiram = ((uintptr_t)s_renderer.canvas_buffer >= 0x40000000 && 
                      (uintptr_t)s_renderer.canvas_buffer < 0x50000000);
    
    // Store SPIRAM flag for later use in cache sync
    s_renderer.canvas_buffer_spiram = is_spiram;
    
    if (!s_renderer.canvas_buffer) {
        ESP_LOGE(TAG, "Failed to allocate canvas buffer (%zu bytes)", s_renderer.canvas_buffer_size);
        return ESP_ERR_NO_MEM;
    }

    // Clear buffer to prevent blue flashing (uninitialized memory)
    memset(s_renderer.canvas_buffer, 0, s_renderer.canvas_buffer_size);

    ESP_LOGI(TAG, "Created canvas buffer: %zux%zu, %zu bytes (%s)",
             (size_t)width, (size_t)height, s_renderer.canvas_buffer_size,
             s_renderer.canvas_buffer_spiram ? "SPIRAM" : "internal");

    return ESP_OK;
}

esp_err_t renderer_init(const renderer_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_renderer.initialized) {
        ESP_LOGW(TAG, "Renderer already initialized");
        return ESP_OK;
    }

    // Create mutex
    s_renderer.mutex = xSemaphoreCreateMutex();
    if (!s_renderer.mutex) {
        return ESP_ERR_NO_MEM;
    }

    s_renderer.canvas = config->canvas;

    // Scan for animation files
    if (!storage_fs_is_sd_present()) {
        ESP_LOGW(TAG, "SD card not present, deferring animation scan");
        s_renderer.initialized = true;
        return ESP_OK;
    }

    const char *sd_path = storage_fs_get_sd_path();
    char anim_dir[256];
    snprintf(anim_dir, sizeof(anim_dir), "%s/animations", sd_path);

    esp_err_t ret = scan_animation_directory(anim_dir);
    if (ret != ESP_OK) {
        // Try fallback directory
        ESP_LOGW(TAG, "Animation directory not found, trying root SD directory");
        ret = scan_animation_directory(sd_path);
    }

    // Initialize video player FIRST (before loading animations)
    // This ensures stripe buffers are allocated early
    esp_err_t vp_ret = video_player_init();
    if (vp_ret == ESP_OK) {
        ESP_LOGI(TAG, "Video player initialized - will use LVGL bypass mode for animations");
        s_renderer.use_video_player = true;
    } else {
        ESP_LOGW(TAG, "Video player init failed, will use LVGL mode: %s", esp_err_to_name(vp_ret));
        s_renderer.use_video_player = false;
    }
    
    // Initialize PPA for hardware-accelerated scaling
    init_ppa();
    if (s_renderer.ppa_available) {
        ESP_LOGI(TAG, "[PPA] Hardware acceleration enabled for scaling");
    } else {
        ESP_LOGW(TAG, "[PPA] Hardware acceleration not available, using software scaling");
    }

    if (ret == ESP_OK && s_renderer.count > 0) {
        // If using video player, skip canvas buffer creation (video player uses internal stripes)
        if (!s_renderer.use_video_player) {
            // Ensure canvas is properly sized before creating buffer
            if (s_renderer.canvas) {
                lv_coord_t width = lv_obj_get_width(s_renderer.canvas);
                lv_coord_t height = lv_obj_get_height(s_renderer.canvas);
                if (width <= 0 || height <= 0) {
                    // Canvas not sized yet, set default size and force layout
                    width = 720;
                    height = 720;
                    if (bsp_display_lock(1000) == pdTRUE) {
                        lv_obj_set_size(s_renderer.canvas, width, height);
                        lv_obj_update_layout(s_renderer.canvas);  // Force layout update
                        bsp_display_unlock();
                    }
                    // Small delay to ensure layout is applied
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
            }
            
            // Create canvas buffer (only needed for LVGL mode)
            ret = create_canvas_buffer();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to create canvas buffer, cannot load animations");
                s_renderer.initialized = true;
                return ret;
            }
            
            // Set canvas buffer (must be done with LVGL locked)
            if (s_renderer.canvas && s_renderer.canvas_buffer) {
                lv_coord_t width = lv_obj_get_width(s_renderer.canvas);
                lv_coord_t height = lv_obj_get_height(s_renderer.canvas);
                if (width <= 0) width = 720;
                if (height <= 0) height = 720;
                
                if (bsp_display_lock(1000) == pdTRUE) {
                    lv_canvas_set_buffer(s_renderer.canvas, s_renderer.canvas_buffer,
                                          width, height, LV_COLOR_FORMAT_RGB888);
                    bsp_display_unlock();
                }
            }
        }

        // Load first animation - video player will start automatically if enabled
        s_renderer.current_index = 0;
        ret = load_animation_file(s_renderer.files[0].path);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to load first animation: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "First animation loaded - playback started");
        }
    } else {
        ESP_LOGW(TAG, "No animation files found");
    }

    s_renderer.initialized = true;
    s_renderer.current_fps = 0.0f;
    s_renderer.decode_time_us = 0;
    s_renderer.blit_time_us = 0;
    s_renderer.flush_time_us = 0;
    s_renderer.frame_interval_us = 0;
    s_renderer.frame_count_profile = 0;
    s_renderer.last_profile_log_us = esp_timer_get_time();

    ESP_LOGI(TAG, "Renderer initialized: %zu animations found", s_renderer.count);
    return ESP_OK;
}

void renderer_cycle_next(void)
{
    ESP_LOGI(TAG, "[RENDERER] renderer_cycle_next() called");
    ESP_LOGI(TAG, "[RENDERER]   initialized=%d, count=%zu, current_index=%zu, video_player_is_playing=%d", 
             s_renderer.initialized, s_renderer.count, s_renderer.current_index, video_player_is_playing());
    
    if (!s_renderer.initialized || s_renderer.count == 0) {
        ESP_LOGW(TAG, "[RENDERER] renderer_cycle_next() ignored: initialized=%d, count=%zu", 
                 s_renderer.initialized, s_renderer.count);
        return;
    }

    // Debounce: prevent rapid-fire cycles (minimum 200ms between cycles)
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_renderer.last_cycle_time_us < 200000) {  // 200ms debounce
        ESP_LOGD(TAG, "[RENDERER] Cycle request debounced (too soon after last cycle)");
        return;
    }

    if (xSemaphoreTake(s_renderer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_renderer.pending_cycle = true;
        ESP_LOGI(TAG, "[RENDERER] Set pending_cycle=true");
        xSemaphoreGive(s_renderer.mutex);
    } else {
        ESP_LOGW(TAG, "[RENDERER] Failed to take mutex for pending_cycle");
    }
}

bool renderer_is_ready(void)
{
    return s_renderer.initialized && 
           s_renderer.count > 0 && 
           s_renderer.decoder != NULL;
}

esp_err_t renderer_get_status(renderer_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_renderer.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    status->fps = s_renderer.current_fps;
    status->animation_count = s_renderer.count;
    status->current_index = s_renderer.current_index;
    status->is_playing = (s_renderer.decoder != NULL);
    
    if (s_renderer.current_index < s_renderer.count) {
        status->current_animation = s_renderer.files[s_renderer.current_index].name;
    } else {
        status->current_animation = NULL;
    }

    xSemaphoreGive(s_renderer.mutex);
    return ESP_OK;
}

void renderer_update(void)
{
    static uint32_t call_count = 0;
    call_count++;
    if (call_count % 1000 == 0) {
        ESP_LOGD(TAG, "[RENDERER] renderer_update() called (count=%lu, video_player_is_playing=%d, use_video_player=%d)", 
                 call_count, video_player_is_playing(), s_renderer.use_video_player);
    }
    
    if (!s_renderer.initialized) {
        return;
    }

    // Always check for pending cycle requests first (regardless of video player state)
    bool do_cycle = false;
    if (xSemaphoreTake(s_renderer.mutex, 0) == pdTRUE) {
        if (s_renderer.pending_cycle) {
            do_cycle = true;
            s_renderer.pending_cycle = false;
            ESP_LOGI(TAG, "[RENDERER] renderer_update() detected pending_cycle=true");
        }
        xSemaphoreGive(s_renderer.mutex);
    }

    if (do_cycle && s_renderer.count > 0) {
        ESP_LOGI(TAG, "[RENDERER] Processing cycle request (video_player_is_playing=%d, use_video_player=%d, count=%zu)", 
                 video_player_is_playing(), s_renderer.use_video_player, s_renderer.count);
        
        // Cycle to next animation index
        size_t old_index = s_renderer.current_index;
        s_renderer.current_index = (s_renderer.current_index + 1) % s_renderer.count;
        s_renderer.last_cycle_time_us = esp_timer_get_time();  // Update cycle timestamp
        ESP_LOGI(TAG, "[RENDERER] Cycling from index %zu to %zu", old_index, s_renderer.current_index);
        ESP_LOGI(TAG, "[RENDERER] Loading animation: %s", s_renderer.files[s_renderer.current_index].name);
        
        // Use unified video player API for seamless switching
        if (s_renderer.use_video_player) {
            if (video_player_is_playing()) {
                ESP_LOGI(TAG, "[RENDERER] Stopping current video player for seamless switch");
                video_player_stop(true);  // Keep bypass mode active
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            
            // Use unified play_file API which auto-detects format and handles file I/O
            ESP_LOGI(TAG, "[RENDERER] Starting new animation with unified API: %s", 
                     s_renderer.files[s_renderer.current_index].path);
            esp_err_t vp_ret = video_player_play_file(s_renderer.files[s_renderer.current_index].path, true);
            
            if (vp_ret == ESP_OK) {
                ESP_LOGI(TAG, "[RENDERER] Animation started successfully");
                return;
            } else {
                ESP_LOGW(TAG, "[RENDERER] Video player start failed: %s", esp_err_to_name(vp_ret));
                s_renderer.current_index = old_index;  // Revert on error
                return;
            }
        } else {
            // Load file for LVGL mode (WebP only)
            const char *file_path = s_renderer.files[s_renderer.current_index].path;
            const char *ext = strrchr(file_path, '.');
            if (ext && strcasecmp(ext, ".gif") == 0) {
                ESP_LOGW(TAG, "[RENDERER] GIF files require video player mode, skipping");
                s_renderer.current_index = old_index;  // Revert
                return;
            }
            
            // Load WebP file for LVGL mode (original code path)
            FILE *f = fopen(file_path, "rb");
            if (!f) {
                ESP_LOGE(TAG, "[RENDERER] Failed to open file: %s", file_path);
                s_renderer.current_index = old_index;
                return;
            }
            
            fseek(f, 0, SEEK_END);
            long file_size = ftell(f);
            fseek(f, 0, SEEK_SET);
            
            if (file_size <= 0 || file_size > 10 * 1024 * 1024) {
                fclose(f);
                ESP_LOGE(TAG, "[RENDERER] Invalid file size: %ld", file_size);
                s_renderer.current_index = old_index;
                return;
            }
            
            uint8_t *new_data = (uint8_t *)malloc((size_t)file_size);
            if (!new_data) {
                fclose(f);
                ESP_LOGE(TAG, "[RENDERER] Failed to allocate memory");
                s_renderer.current_index = old_index;
                return;
            }
            
            size_t read = fread(new_data, 1, (size_t)file_size, f);
            fclose(f);
            
            if (read != (size_t)file_size) {
                free(new_data);
                ESP_LOGE(TAG, "[RENDERER] Failed to read file");
                s_renderer.current_index = old_index;
                return;
            }
            
            // Use video player for WebP too (for consistency)
            if (video_player_is_playing()) {
                video_player_stop(true);
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            
            esp_err_t vp_ret = video_player_play_webp(new_data, (size_t)file_size, true);
            if (vp_ret != ESP_OK) {
                free(new_data);
                ESP_LOGW(TAG, "[RENDERER] Video player start failed: %s", esp_err_to_name(vp_ret));
                s_renderer.current_index = old_index;
                return;
            }
        }
        
        ESP_LOGI(TAG, "[RENDERER] Cycled to animation: %s (video_player_is_playing=%d, use_video_player=%d)", 
                 s_renderer.files[s_renderer.current_index].name, video_player_is_playing(), s_renderer.use_video_player);
        return;
    }

    // If video player is active, skip LVGL rendering
    if (video_player_is_playing()) {
        return;  // Video player handles playback in bypass mode
    }

    // If using video player but it's not playing, don't render via LVGL
    // But add a small delay to prevent watchdog timeout
    if (s_renderer.use_video_player && !video_player_is_playing()) {
        vTaskDelay(pdMS_TO_TICKS(10));  // Small delay to prevent watchdog timeout
        return;  // Wait for video player to be started
    }

    // Check if we have a valid animation and canvas (for LVGL mode)
    if (!s_renderer.decoder || !s_renderer.canvas || !s_renderer.canvas_buffer) {
        return;
    }

    // Check frame timing - ignore WebP delays, run as fast as possible
    int64_t now_us = esp_timer_get_time();
    int64_t elapsed_us = now_us - s_renderer.last_frame_time_us;
    
    // Minimum frame interval for 30 FPS: ~33.3ms (33333us)
    // But we want to run faster, so use 1ms minimum to allow maximum throughput
    const int64_t min_frame_interval_us = 1000;  // 1ms minimum
    
    if (elapsed_us < min_frame_interval_us) {
        return;  // Not time for next frame yet
    }

    // Profile: Start timing
    int64_t frame_start_us = esp_timer_get_time();
    int64_t decode_start_us = frame_start_us;

    // Decode next frame
    uint8_t *frame_rgb = NULL;
    int timestamp_ms = 0;

    if (!WebPAnimDecoderGetNext(s_renderer.decoder, &frame_rgb, &timestamp_ms)) {
        // Loop animation
        WebPAnimDecoderReset(s_renderer.decoder);
        s_renderer.frame_index = 0;
        if (!WebPAnimDecoderGetNext(s_renderer.decoder, &frame_rgb, &timestamp_ms)) {
            ESP_LOGE(TAG, "Failed to restart animation");
            return;
        }
    }

    if (!frame_rgb) {
        return;
    }

    // Profile: Decode time
    int64_t decode_end_us = esp_timer_get_time();
    s_renderer.decode_time_us = decode_end_us - decode_start_us;
    int64_t blit_start_us = decode_end_us;

    // Render frame to canvas buffer - fullscreen scaling
    lv_coord_t canvas_w = lv_obj_get_width(s_renderer.canvas);
    lv_coord_t canvas_h = lv_obj_get_height(s_renderer.canvas);
    if (canvas_w <= 0) canvas_w = 720;
    if (canvas_h <= 0) canvas_h = 720;

    int src_w = (int)s_renderer.anim_info.canvas_width;
    int src_h = (int)s_renderer.anim_info.canvas_height;
    int dst_w = (int)canvas_w;
    int dst_h = (int)canvas_h;

    bool needs_scaling = (src_w != dst_w) || (src_h != dst_h);
    esp_err_t blit_ret = ESP_FAIL;

    if (needs_scaling && s_renderer.ppa_available) {
        // Prepare DMA-friendly source buffer and use PPA for scaling
        if (ensure_source_rgb888_buffer(src_w, src_h) == ESP_OK) {
            copy_rgba_to_rgb888_fast(frame_rgb, src_w, src_h, s_renderer.source_rgb888_buffer);
            blit_ret = blit_rgb888_with_ppa(s_renderer.source_rgb888_buffer, src_w, src_h,
                                            s_renderer.canvas_buffer, dst_w, dst_h);
            if (blit_ret == ESP_OK) {
                s_renderer.using_ppa = true;
            } else {
                ESP_LOGW(TAG, "[PPA] Hardware scaling failed, falling back to software: %s",
                         esp_err_to_name(blit_ret));
            }
        } else {
            ESP_LOGW(TAG, "[PPA] Failed to allocate RGB888 staging buffer, falling back to software");
        }
    }

    if (blit_ret != ESP_OK) {
        // Software path (also handles 1:1 copy)
        s_renderer.using_ppa = false;
        ESP_LOGD(TAG, "[RENDER] Using software nearest neighbor scaling: %dx%d -> %dx%d",
                 src_w, src_h, dst_w, dst_h);
        blit_rgba_to_rgb888(frame_rgb, src_w, src_h,
                            s_renderer.canvas_buffer, dst_w, dst_h);
    }

    // Profile: Blit time
    int64_t blit_end_us = esp_timer_get_time();
    s_renderer.blit_time_us = blit_end_us - blit_start_us;
    int64_t flush_start_us = blit_end_us;

    // Optimized flush/display transfer:
    // 1. For SPIRAM buffers: Ensure cache is synchronized before LVGL accesses it
    // 2. Use minimal lock time - just invalidate canvas, let LVGL handle the transfer
    // 3. Avoid blocking on flush - LVGL will handle the DMA transfer asynchronously
    
    // Sync cache for SPIRAM buffers before LVGL accesses it
    // This ensures DMA-2D can see the updated data without cache misses
    if (s_renderer.canvas_buffer_spiram) {
        // For SPIRAM, we need to ensure cache coherency
        // However, LVGL's flush callback will handle the actual transfer
        // We just need to mark the buffer as dirty
        lv_draw_buf_t *draw_buf = lv_canvas_get_draw_buf(s_renderer.canvas);
        if (draw_buf) {
            // Mark buffer as dirty - LVGL will handle cache sync during flush
            lv_draw_buf_flush_cache(draw_buf, NULL);
        }
    }
    
    // Invalidate canvas area to trigger redraw - this is non-blocking
    // LVGL will handle the actual DMA transfer to display in the flush callback
    if (bsp_display_lock(10) == pdTRUE) {  // Very short timeout - just invalidate
        lv_obj_invalidate(s_renderer.canvas);
        bsp_display_unlock();
    }

    // Profile: Flush time (includes cache sync + invalidation, but NOT DMA wait)
    // The actual DMA transfer happens asynchronously in LVGL's flush callback
    int64_t flush_end_us = esp_timer_get_time();
    s_renderer.flush_time_us = flush_end_us - flush_start_us;
    
    // Profile: Frame interval
    s_renderer.frame_interval_us = now_us - s_renderer.last_frame_time_us;
    s_renderer.frame_count_profile++;
    
    // Log profiling stats every 60 frames (~2 seconds at 30 FPS)
    int64_t current_time_us = esp_timer_get_time();
    if (current_time_us - s_renderer.last_profile_log_us >= 2000000) {  // 2 seconds
        float avg_fps = (s_renderer.frame_count_profile * 1000000.0f) / 
                       (current_time_us - s_renderer.last_profile_log_us);
        
        // Calculate frame time breakdown
        int64_t total_frame_time_us = s_renderer.decode_time_us + s_renderer.blit_time_us + s_renderer.flush_time_us;
        float decode_pct = (s_renderer.decode_time_us * 100.0f) / total_frame_time_us;
        float blit_pct = (s_renderer.blit_time_us * 100.0f) / total_frame_time_us;
        float flush_pct = (s_renderer.flush_time_us * 100.0f) / total_frame_time_us;
        
        ESP_LOGI(TAG, "=== PERFORMANCE PROFILE ===");
        ESP_LOGI(TAG, "FPS: %.1f (target: 30.0)", avg_fps);
        ESP_LOGI(TAG, "Frame interval: %" PRId64 "us (target: 33333us for 30 FPS)", s_renderer.frame_interval_us);
        ESP_LOGI(TAG, "Frame time breakdown:");
        ESP_LOGI(TAG, "  Decode: %" PRId64 "us (%.1f%%)", s_renderer.decode_time_us, decode_pct);
        ESP_LOGI(TAG, "  Blit:   %" PRId64 "us (%.1f%%)", s_renderer.blit_time_us, blit_pct);
        ESP_LOGI(TAG, "  Flush:  %" PRId64 "us (%.1f%%)", s_renderer.flush_time_us, flush_pct);
        ESP_LOGI(TAG, "  Total:  %" PRId64 "us", total_frame_time_us);
        ESP_LOGI(TAG, "Animation: %ux%u -> Canvas: 720x720", 
                 (unsigned)s_renderer.anim_info.canvas_width,
                 (unsigned)s_renderer.anim_info.canvas_height);
        ESP_LOGI(TAG, "Buffer: %s (size: %zu bytes)", 
                 s_renderer.canvas_buffer_spiram ? "SPIRAM" : "INTERNAL/DMA",
                 s_renderer.canvas_buffer_size);
        ESP_LOGI(TAG, "PPA: %s (hardware acceleration: %s)", 
                 s_renderer.ppa_available ? "Available" : "Not available",
                 s_renderer.using_ppa ? "ACTIVE" : "inactive");
        ESP_LOGI(TAG, "Scaling: Software nearest neighbor (crisp, pixelated)");
        ESP_LOGI(TAG, "========================");
        
        s_renderer.current_fps = avg_fps;
        s_renderer.frame_count_profile = 0;
        s_renderer.last_profile_log_us = current_time_us;
    }

    // Update timing - ignore WebP frame delays
    s_renderer.frame_index = (s_renderer.frame_index + 1) % s_renderer.frame_count;
    s_renderer.current_frame_delay_ms = 1;  // Force minimum delay (1ms)
    s_renderer.last_frame_time_us = now_us;
}