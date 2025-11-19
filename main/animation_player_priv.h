#ifndef ANIMATION_PLAYER_PRIV_H
#define ANIMATION_PLAYER_PRIV_H

#include "animation_player.h"
#include "animation_decoder.h"
#include "app_lcd.h"
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
#include "bsp/esp-bsp.h"
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

#define PICO8_FRAME_WIDTH        128
#define PICO8_FRAME_HEIGHT       128
#define PICO8_PALETTE_COLORS     16
#define PICO8_FRAME_BYTES        (PICO8_FRAME_WIDTH * PICO8_FRAME_HEIGHT / 2)
#define PICO8_STREAM_TIMEOUT_US  (250 * 1000)

#define ANIMATIONS_PREFERRED_DIR "/sdcard/animations"
#define ANIMATION_SD_REFRESH_STACK (16384)

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} pico8_color_t;

typedef enum {
    ASSET_TYPE_WEBP,
    ASSET_TYPE_GIF,
    ASSET_TYPE_PNG,
    ASSET_TYPE_JPEG,
} asset_type_t;

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
    uint8_t *native_frame_b1;
    uint8_t *native_frame_b2;
    uint8_t native_buffer_active;
    size_t native_frame_size;
    uint16_t *upscale_lookup_x;
    uint16_t *upscale_lookup_y;
    int upscale_src_w, upscale_src_h;
    int upscale_dst_w, upscale_dst_h;
    uint8_t *prefetched_first_frame;
    bool first_frame_ready;
    bool decoder_at_frame_1;
    bool prefetch_pending;
    uint32_t prefetched_first_frame_delay_ms;
    uint32_t current_frame_delay_ms;
    bool ready;
} animation_buffer_t;

extern esp_lcd_panel_handle_t s_display_handle;
extern uint8_t **s_lcd_buffers;
extern uint8_t s_buffer_count;
extern size_t s_frame_buffer_bytes;
extern size_t s_frame_row_stride_bytes;

extern SemaphoreHandle_t s_vsync_sem;
extern TaskHandle_t s_anim_task;

extern animation_buffer_t s_front_buffer;
extern animation_buffer_t s_back_buffer;
extern size_t s_next_asset_index;
extern bool s_swap_requested;
extern bool s_loader_busy;
extern TaskHandle_t s_loader_task;
extern SemaphoreHandle_t s_loader_sem;
extern SemaphoreHandle_t s_buffer_mutex;

extern bool s_anim_paused;

extern TaskHandle_t s_upscale_worker_top;
extern TaskHandle_t s_upscale_worker_bottom;
extern TaskHandle_t s_upscale_main_task;
extern const uint8_t *s_upscale_src_buffer;
extern uint8_t *s_upscale_dst_buffer;
extern const uint16_t *s_upscale_lookup_x;
extern const uint16_t *s_upscale_lookup_y;
extern int s_upscale_src_w;
extern int s_upscale_src_h;
extern int s_upscale_row_start_top;
extern int s_upscale_row_end_top;
extern int s_upscale_row_start_bottom;
extern int s_upscale_row_end_bottom;
extern volatile bool s_upscale_worker_top_done;
extern volatile bool s_upscale_worker_bottom_done;

extern uint8_t s_render_buffer_index;
extern uint8_t s_last_display_buffer;

extern int64_t s_last_frame_present_us;
extern int64_t s_last_duration_update_us;
extern int s_latest_frame_duration_ms;
extern char s_frame_duration_text[11];
extern int64_t s_frame_processing_start_us;
extern uint32_t s_target_frame_delay_ms;

extern app_lcd_sd_file_list_t s_sd_file_list;
extern bool s_sd_mounted;
extern bool s_sd_export_active;

esp_err_t load_animation_into_buffer(size_t asset_index, animation_buffer_t *buf);
void unload_animation_buffer(animation_buffer_t *buf);
esp_err_t prefetch_first_frame(animation_buffer_t *buf);
void animation_loader_task(void *arg);
void animation_loader_wait_for_idle(void);

bool directory_has_animation_files(const char *dir_path);
esp_err_t find_animations_directory(const char *root_path, char **found_dir_out);
esp_err_t enumerate_animation_files(const char *dir_path);
void free_sd_file_list(void);
esp_err_t refresh_animation_file_list(void);
size_t get_next_asset_index(size_t current_index);
size_t get_previous_asset_index(size_t current_index);

esp_err_t ensure_pico8_resources(void);
void release_pico8_resources(void);
bool pico8_stream_should_render(void);
int render_pico8_frame(uint8_t *dest_buffer);

bool lcd_panel_refresh_done_cb(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx);
void lcd_animation_task(void *arg);
void upscale_worker_top_task(void *arg);
void upscale_worker_bottom_task(void *arg);

#endif // ANIMATION_PLAYER_PRIV_H

