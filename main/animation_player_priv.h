// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#ifndef ANIMATION_PLAYER_PRIV_H
#define ANIMATION_PLAYER_PRIV_H

#include "animation_player.h"
#include "animation_decoder.h"
#include "display_renderer.h"
#include "playback_controller.h"
#include "animation_metadata.h"
#include "p3a_board.h"
#include "app_lcd.h"
#include "sdcard_channel.h"  // For asset_type_t (canonical definition)
#include "config_store.h"
#include "sd_path.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_lcd_panel_ops.h"
#include "bsp/esp-bsp.h"  // For bsp_sdcard_mount/unmount, BSP_SD_MOUNT_POINT
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#ifndef __has_include
#define __has_include(x) 0
#endif

#if __has_include("esp_cache.h")
#include "esp_cache.h"
#define APP_LCD_HAVE_CACHE_MSYNC 1
#else
#define APP_LCD_HAVE_CACHE_MSYNC 0
#endif

#if defined(__XTENSA__)
#include "xtensa/hal.h"
#define MEMORY_BARRIER() xthal_dcache_sync()
#elif defined(__riscv)
#include "riscv/rv_utils.h"
#define MEMORY_BARRIER() __asm__ __volatile__ ("fence" ::: "memory")
#else
#define MEMORY_BARRIER() __asm__ __volatile__ ("" ::: "memory")
#endif

#define TAG "anim_player"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

// Note: ANIMATIONS_PREFERRED_DIR is now deprecated.
// Use sd_path_get_animations() to get the current animations directory path.
// This macro is kept for compatibility but should not be used in new code.
// Note: SD card root is now configurable via sd_path module (default: /p3a)
// This gets combined with SD mount point at runtime
#define ANIMATION_SD_REFRESH_STACK (16384)

// asset_type_t is defined in sdcard_channel.h

// Use display_renderer's rotation type for compatibility
typedef display_rotation_t screen_rotation_t;
#define ROTATION_0   DISPLAY_ROTATION_0
#define ROTATION_90  DISPLAY_ROTATION_90
#define ROTATION_180 DISPLAY_ROTATION_180
#define ROTATION_270 DISPLAY_ROTATION_270

typedef struct {
    char **filenames;
    asset_type_t *types;
    bool *health_flags;
    size_t count;
    size_t current_index;
    char *animations_dir;
} app_lcd_sd_file_list_t;

typedef struct {
    animation_decoder_t *decoder;
    const uint8_t *file_data;
    size_t file_size;
    animation_decoder_info_t decoder_info;
    asset_type_t type;
    size_t asset_index;
    uint8_t *native_frame_b1;           // Native decoded frame buffer 1
    uint8_t *native_frame_b2;           // Native decoded frame buffer 2
    uint8_t native_buffer_active;       // Which native buffer is active (0 or 1)
    uint8_t native_bytes_per_pixel;     // 3 (RGB888) or 4 (RGBA8888)
    size_t native_frame_size;
    uint16_t *upscale_lookup_x;
    uint16_t *upscale_lookup_y;
    int upscale_src_w, upscale_src_h;
    int upscale_dst_w, upscale_dst_h;

    // Aspect ratio preservation / borders (computed when building lookup tables)
    int upscale_offset_x;      // X offset for centering (border on left)
    int upscale_offset_y;      // Y offset for centering (border on top)
    int upscale_scaled_w;      // Scaled image width (< dst_w if letterboxed)
    int upscale_scaled_h;      // Scaled image height (< dst_h if pillarboxed)
    bool upscale_has_borders;  // True if aspect ratio doesn't match; draw borders with bg color
    display_rotation_t upscale_rotation_built;  // Rotation used when building lookup tables

    bool first_frame_ready;             // First frame decoded and ready in native_frame_b1
    bool decoder_at_frame_1;            // Decoder has advanced past frame 0
    bool prefetch_pending;              // Prefetch decode requested but not yet done
    bool prefetch_in_progress;          // Prefetch is currently executing (render task using buffers)
    uint32_t prefetched_first_frame_delay_ms;  // Frame delay for the prefetched first frame
    uint32_t current_frame_delay_ms;
    bool ready;
    char *filepath;  // Path to the animation file

    // Static frame caching (frame_count <= 1):
    // - Keep native_frame_b1 as the canonical decoded frame and reuse it every render tick
    // - If background color changes and the asset has transparency, re-decode once to refresh compositing
    bool static_frame_cached;
    uint32_t static_bg_generation;

    // Live Mode / swap_future start alignment
    // - start_time_ms: ideal wall-clock time when this animation "started" (UTC, ms since epoch).
    //   Prefetch will align by seeking to (now_ms - start_time_ms).
    // - start_frame: optional explicit frame seek (frame index); used when start_time_ms == 0.
    uint64_t start_time_ms;
    uint32_t start_frame;

    // Live Mode swap context (for recovery)
    bool is_live_mode_swap;
    uint32_t live_index;
    
    // View tracking: post_id of the artwork being displayed
    int32_t post_id;
} animation_buffer_t;

// Animation player state (now delegates to display_renderer for LCD operations)
extern animation_buffer_t s_front_buffer;
extern animation_buffer_t s_back_buffer;
extern size_t s_next_asset_index;
extern bool s_swap_requested;
extern bool s_loader_busy;
extern volatile bool s_cycle_pending;
extern volatile bool s_cycle_forward;
extern TaskHandle_t s_loader_task;
extern SemaphoreHandle_t s_loader_sem;
extern SemaphoreHandle_t s_buffer_mutex;
extern SemaphoreHandle_t s_prefetch_done_sem;  // Signaled when prefetch completes

// Override for the next load triggered by swap_future_execute().
// If valid, the loader will load this filepath/type instead of play_scheduler current item,
// and will propagate start_time_ms/start_frame to the back buffer for prefetch alignment.
typedef struct {
    bool valid;
    char filepath[256];
    asset_type_t type;
    uint64_t start_time_ms;
    uint32_t start_frame;
    bool is_live_mode_swap;
    uint32_t live_index;
    int32_t post_id;  // For view tracking
} animation_load_override_t;

extern animation_load_override_t s_load_override;

extern bool s_anim_paused;

extern app_lcd_sd_file_list_t s_sd_file_list;
extern bool s_sd_mounted;
extern bool s_sd_export_active;

// Animation loading functions
esp_err_t load_animation_into_buffer(const char *filepath, asset_type_t type, animation_buffer_t *buf,
                                     uint32_t start_frame, uint64_t start_time_ms);
void unload_animation_buffer(animation_buffer_t *buf);
esp_err_t prefetch_first_frame(animation_buffer_t *buf);
void animation_loader_task(void *arg);
void animation_loader_wait_for_idle(void);
void animation_loader_mark_swap_successful(void);
bool animation_loader_try_delete_corrupt_vault_file(const char *filepath, esp_err_t error);

// Rebuild aspect-ratio/rotation-dependent upscale maps for an already-loaded buffer.
// Call from the render task (or otherwise ensure it doesn't race with render_next_frame()).
esp_err_t animation_loader_rebuild_upscale_maps(animation_buffer_t *buf, display_rotation_t rotation);

// Directory enumeration
bool directory_has_animation_files(const char *dir_path);
esp_err_t find_animations_directory(const char *root_path, char **found_dir_out);
esp_err_t enumerate_animation_files(const char *dir_path);
void free_sd_file_list(void);
esp_err_t refresh_animation_file_list(void);
size_t get_next_asset_index(size_t current_index);
size_t get_previous_asset_index(size_t current_index);

// Frame rendering callback (called by display_renderer)
int animation_player_render_frame_callback(uint8_t *dest_buffer, void *user_ctx);

// Notify the render task that rotation changed; it will rebuild lookup maps at a safe point.
void animation_player_render_on_rotation_changed(display_rotation_t rotation);

#endif // ANIMATION_PLAYER_PRIV_H
