#include "playback_engine.h"

#include "p3a_hal/display.h"
#include "bsp/display.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_4b.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "webp/demux.h"
#include "webp/decode.h"
#include "video_player.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#define TAG "playback_engine"

#define FRAME_WIDTH  BSP_LCD_H_RES
#define FRAME_HEIGHT BSP_LCD_V_RES
#define FRAME_PERIOD_MS 33
#define CACHE_LINE_SIZE 64
#define CMD_QUEUE_LENGTH 4

typedef enum {
    PLAYBACK_CMD_START,
    PLAYBACK_CMD_SWITCH,
    PLAYBACK_CMD_STOP,
} playback_cmd_type_t;

typedef struct {
    playback_cmd_type_t type;
    char path[256];
    SemaphoreHandle_t ack;
    esp_err_t result;
} playback_command_t;

typedef struct {
    WebPAnimDecoder *decoder;
    WebPAnimInfo info;
    uint8_t *file_data;
    size_t file_size;
    int *x_map;
    int *y_map;
    bool direct_copy;
    bool is_gif;  // true if using video_player for GIF
} playback_animation_t;

typedef struct {
    QueueHandle_t queue;
    TaskHandle_t task;
    uint8_t *frame_buffer;
    size_t frame_buffer_bytes;
    bool frame_buffer_spiram;
    lv_display_t *lv_display;
    esp_lcd_panel_handle_t panel;
    SemaphoreHandle_t trans_sem;
    bool bypass_active;
    bool running;
    TickType_t frame_period_ticks;
    TickType_t next_frame_tick;
    playback_animation_t current;
} playback_engine_ctx_t;

static playback_engine_ctx_t s_ctx = {0};

static esp_err_t acquire_display(playback_engine_ctx_t *ctx);
static esp_err_t release_display(playback_engine_ctx_t *ctx);
static esp_err_t load_animation(playback_animation_t *anim, const char *path);
static void unload_animation(playback_animation_t *anim);
static esp_err_t render_next_frame(playback_engine_ctx_t *ctx);
static esp_err_t start_playback_locked(playback_engine_ctx_t *ctx, const char *path);
static void playback_task(void *arg);
static esp_err_t send_command(playback_cmd_type_t type, const char *path);

esp_err_t playback_engine_init(void)
{
    if (s_ctx.queue) {
        return ESP_OK;
    }

    // Initialize video_player for GIF support
    esp_err_t vp_err = video_player_init();
    if (vp_err != ESP_OK) {
        ESP_LOGW(TAG, "video_player init failed: %s (GIF files won't be supported)", esp_err_to_name(vp_err));
    }

    size_t buffer_size = (size_t)FRAME_WIDTH * FRAME_HEIGHT * 3;
    s_ctx.frame_buffer = (uint8_t *)heap_caps_aligned_alloc(
        CACHE_LINE_SIZE,
        buffer_size,
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    if (!s_ctx.frame_buffer) {
        ESP_LOGW(TAG, "DMA-capable allocation failed, trying internal memory");
        s_ctx.frame_buffer = (uint8_t *)heap_caps_aligned_alloc(
            CACHE_LINE_SIZE,
            buffer_size,
            MALLOC_CAP_INTERNAL);
    }

    if (!s_ctx.frame_buffer) {
        ESP_LOGW(TAG, "Internal memory allocation failed, trying SPIRAM");
        s_ctx.frame_buffer = (uint8_t *)heap_caps_aligned_alloc(
            CACHE_LINE_SIZE,
            buffer_size,
            MALLOC_CAP_SPIRAM);
    }

    if (!s_ctx.frame_buffer) {
        ESP_LOGE(TAG, "Failed to allocate playback frame buffer (%zu bytes)", buffer_size);
        return ESP_ERR_NO_MEM;
    }
    s_ctx.frame_buffer_bytes = buffer_size;
    s_ctx.frame_buffer_spiram = ((uintptr_t)s_ctx.frame_buffer >= 0x40000000 && 
                                  (uintptr_t)s_ctx.frame_buffer < 0x50000000);

    s_ctx.queue = xQueueCreate(CMD_QUEUE_LENGTH, sizeof(playback_command_t));
    if (!s_ctx.queue) {
        ESP_LOGE(TAG, "Failed to create playback command queue");
        free(s_ctx.frame_buffer);
        s_ctx.frame_buffer = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_ctx.frame_period_ticks = pdMS_TO_TICKS(FRAME_PERIOD_MS);
    if (s_ctx.frame_period_ticks == 0) {
        s_ctx.frame_period_ticks = 1;
    }

    if (xTaskCreatePinnedToCore(
            playback_task,
            "playback_runner",
            8192,
            NULL,
            5,
            &s_ctx.task,
            1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create playback task");
        vQueueDelete(s_ctx.queue);
        s_ctx.queue = NULL;
        free(s_ctx.frame_buffer);
        s_ctx.frame_buffer = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Playback engine initialised");
    return ESP_OK;
}

esp_err_t playback_engine_start(const char *path)
{
    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_command(PLAYBACK_CMD_START, path);
}

esp_err_t playback_engine_switch(const char *path)
{
    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }
    return send_command(PLAYBACK_CMD_SWITCH, path);
}

esp_err_t playback_engine_stop(void)
{
    if (!s_ctx.queue) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ctx.running && s_ctx.current.is_gif) {
        ESP_LOGI(TAG, "Stopping GIF playback via video_player");
        esp_err_t stop_ret = video_player_stop(false);
        if (stop_ret != ESP_OK) {
            ESP_LOGW(TAG, "video_player_stop returned %s", esp_err_to_name(stop_ret));
        }

        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
        while (video_player_is_playing() && xTaskGetTickCount() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (video_player_is_playing()) {
            ESP_LOGW(TAG, "GIF playback still reported active after stop request");
        }

        s_ctx.running = false;
        return stop_ret;
    }

    return send_command(PLAYBACK_CMD_STOP, NULL);
}

bool playback_engine_is_running(void)
{
    return s_ctx.running;
}

static esp_err_t send_command(playback_cmd_type_t type, const char *path)
{
    if (!s_ctx.queue) {
        return ESP_ERR_INVALID_STATE;
    }

    playback_command_t cmd = {
        .type = type,
        .ack = xSemaphoreCreateBinary(),
        .result = ESP_OK,
    };

    if (!cmd.ack) {
        return ESP_ERR_NO_MEM;
    }

    if (path) {
        strncpy(cmd.path, path, sizeof(cmd.path) - 1);
        cmd.path[sizeof(cmd.path) - 1] = '\0';
    } else {
        cmd.path[0] = '\0';
    }

    if (xQueueSend(s_ctx.queue, &cmd, portMAX_DELAY) != pdPASS) {
        vSemaphoreDelete(cmd.ack);
        return ESP_FAIL;
    }

    if (xSemaphoreTake(cmd.ack, portMAX_DELAY) != pdTRUE) {
        vSemaphoreDelete(cmd.ack);
        return ESP_FAIL;
    }
    vSemaphoreDelete(cmd.ack);
    return cmd.result;
}

static void playback_task(void *arg)
{
    (void)arg;
    playback_engine_ctx_t *ctx = &s_ctx;
    bool playing = false;

    while (1) {
        playback_command_t cmd;
        BaseType_t got_cmd = xQueueReceive(ctx->queue, &cmd, playing ? 0 : portMAX_DELAY);
        if (got_cmd == pdTRUE) {
            esp_err_t result = ESP_OK;
            switch (cmd.type) {
            case PLAYBACK_CMD_START:
                result = start_playback_locked(ctx, cmd.path);
                playing = (result == ESP_OK);
                break;
            case PLAYBACK_CMD_SWITCH:
                if (ctx->running) {
                    result = start_playback_locked(ctx, cmd.path);
                    playing = (result == ESP_OK);
                } else {
                    result = start_playback_locked(ctx, cmd.path);
                    playing = (result == ESP_OK);
                }
                break;
            case PLAYBACK_CMD_STOP:
                if (ctx->running) {
                    if (ctx->current.is_gif) {
                        // Stop video_player
                        video_player_stop(false);  // don't keep bypass
                    } else {
                        unload_animation(&ctx->current);
                        release_display(ctx);
                    }
                    ctx->running = false;
                }
                playing = false;
                result = ESP_OK;
                break;
            }
            cmd.result = result;
            if (cmd.ack) {
                xSemaphoreGive(cmd.ack);
            }
        }

        if (!playing) {
            taskYIELD();
            continue;
        }

        // If using video_player for GIF, skip frame rendering (video_player handles it)
        if (ctx->current.is_gif) {
            // video_player handles playback in its own task
            if (!video_player_is_playing()) {
                // Playback ended
                ctx->running = false;
                playing = false;
            }
            vTaskDelay(pdMS_TO_TICKS(100));  // Don't spin too fast
            continue;
        }

        esp_err_t frame_result = render_next_frame(ctx);
        if (frame_result != ESP_OK) {
            ESP_LOGW(TAG, "Frame render failed: %s", esp_err_to_name(frame_result));
            unload_animation(&ctx->current);
            release_display(ctx);
            ctx->running = false;
            playing = false;
            continue;
        }

        TickType_t now = xTaskGetTickCount();
        if (ctx->next_frame_tick == 0) {
            ctx->next_frame_tick = now + ctx->frame_period_ticks;
        } else {
            ctx->next_frame_tick += ctx->frame_period_ticks;
            if ((int32_t)(ctx->next_frame_tick - now) > 0) {
                vTaskDelay(ctx->next_frame_tick - now);
            } else {
                ctx->next_frame_tick = now;
                taskYIELD();
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
    }
}

static esp_err_t release_display(playback_engine_ctx_t *ctx)
{
    if (!ctx->bypass_active) {
        return ESP_OK;
    }

    if (lvgl_port_resume() != ESP_OK) {
        ESP_LOGW(TAG, "Failed to resume LVGL port");
    }
    bsp_display_unlock();
    ctx->panel = NULL;
    ctx->trans_sem = NULL;
    ctx->bypass_active = false;
    ESP_LOGD(TAG, "Display bypass released");
    return ESP_OK;
}

static bool is_gif_file(const char *path)
{
    if (!path) {
        return false;
    }
    size_t len = strlen(path);
    if (len < 4) {
        return false;
    }
    return strcasecmp(path + len - 4, ".gif") == 0;
}

static esp_err_t start_playback_locked(playback_engine_ctx_t *ctx, const char *path)
{
    if (!path || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    bool is_gif = is_gif_file(path);
    
    // Stop any current playback and release display
    if (ctx->running) {
        if (ctx->current.is_gif) {
            // Stop video_player
            esp_err_t stop_ret = video_player_stop(is_gif);
            if (stop_ret != ESP_OK) {
                ESP_LOGW(TAG, "video_player_stop returned %s", esp_err_to_name(stop_ret));
            }

            TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
            while (video_player_is_playing() && xTaskGetTickCount() < deadline) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }

            if (video_player_is_playing()) {
                ESP_LOGW(TAG, "Timeout waiting for GIF playback to stop");
                return ESP_ERR_TIMEOUT;
            }
        } else {
            unload_animation(&ctx->current);
            release_display(ctx);
        }
        ctx->running = false;
    }

    // Check if it's a GIF file - delegate to video_player
    if (is_gif) {
        // video_player handles its own display bypass
        esp_err_t err = video_player_play_file(path, true);  // loop=true
        if (err == ESP_OK) {
            ctx->running = true;
            // Mark as GIF so we know to use video_player
            ctx->current.is_gif = true;
            ESP_LOGI(TAG, "Started GIF playback via video_player: %s", path);
        }
        return err;
    }

    // WebP file - use playback_engine's own decoder
    esp_err_t err = acquire_display(ctx);
    if (err != ESP_OK) {
        return err;
    }

    unload_animation(&ctx->current);
    err = load_animation(&ctx->current, path);
    if (err != ESP_OK) {
        release_display(ctx);
        return err;
    }

    ctx->running = true;
    ctx->next_frame_tick = xTaskGetTickCount();
    return ESP_OK;
}

static esp_err_t acquire_display(playback_engine_ctx_t *ctx)
{
    if (ctx->bypass_active) {
        return ESP_OK;
    }

    ctx->lv_display = p3a_hal_display_get_handle();
    if (!ctx->lv_display) {
        ESP_LOGE(TAG, "LVGL display not initialised");
        return ESP_ERR_INVALID_STATE;
    }

    if (!bsp_display_lock(portMAX_DELAY)) {
        ESP_LOGE(TAG, "Failed to lock display mutex");
        return ESP_ERR_TIMEOUT;
    }

    typedef struct {
        uint8_t disp_type;
        void *io_handle;
        esp_lcd_panel_handle_t panel_handle;
        void *control_handle;
        lvgl_port_rotation_cfg_t rotation;
        lv_color_t *draw_buffs[3];
        uint8_t *oled_buffer;
        lv_display_t *disp_drv;
        lv_display_rotation_t current_rotation;
        SemaphoreHandle_t trans_sem;
    } lvgl_port_display_ctx_t;

    lvgl_port_display_ctx_t *disp_ctx = (lvgl_port_display_ctx_t *)lv_display_get_driver_data(ctx->lv_display);
    if (!disp_ctx || !disp_ctx->panel_handle) {
        bsp_display_unlock();
        ESP_LOGE(TAG, "Failed to obtain panel handle from LVGL");
        return ESP_ERR_INVALID_STATE;
    }

    ctx->panel = disp_ctx->panel_handle;
    ctx->trans_sem = disp_ctx->trans_sem;
    
    if (lvgl_port_stop() != ESP_OK) {
        ESP_LOGW(TAG, "Failed to stop LVGL port");
    }

    ctx->bypass_active = true;
    ESP_LOGD(TAG, "Display bypass acquired (panel=%p, trans_sem=%p)", (void *)ctx->panel, (void *)ctx->trans_sem);
    return ESP_OK;
}

static esp_err_t load_animation(playback_animation_t *anim, const char *path)
{
    memset(anim, 0, sizeof(*anim));

    // Check if it's a GIF file - delegate to video_player
    if (is_gif_file(path)) {
        anim->is_gif = true;
        ESP_LOGI(TAG, "Detected GIF file, delegating to video_player: %s", path);
        return ESP_OK;  // video_player will handle it
    }

    anim->is_gif = false;

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open animation: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size <= 0 || file_size > 10 * 1024 * 1024) {
        fclose(f);
        ESP_LOGE(TAG, "Invalid animation size: %ld", file_size);
        return ESP_ERR_INVALID_SIZE;
    }

    anim->file_data = (uint8_t *)malloc((size_t)file_size);
    if (!anim->file_data) {
        fclose(f);
        ESP_LOGE(TAG, "Failed to allocate animation buffer (%ld bytes)", file_size);
        return ESP_ERR_NO_MEM;
    }

    size_t read = fread(anim->file_data, 1, (size_t)file_size, f);
    fclose(f);
    if (read != (size_t)file_size) {
        free(anim->file_data);
        anim->file_data = NULL;
        ESP_LOGE(TAG, "Failed to read animation file completely");
        return ESP_FAIL;
    }

    anim->file_size = (size_t)file_size;

    WebPAnimDecoderOptions options;
    if (!WebPAnimDecoderOptionsInit(&options)) {
        unload_animation(anim);
        return ESP_FAIL;
    }
    options.color_mode = MODE_RGBA;
    options.use_threads = 0;

    WebPData webp_data = {
        .bytes = anim->file_data,
        .size = anim->file_size,
    };

    anim->decoder = WebPAnimDecoderNew(&webp_data, &options);
    if (!anim->decoder) {
        unload_animation(anim);
        ESP_LOGE(TAG, "Failed to create WebP decoder");
        return ESP_FAIL;
    }

    if (!WebPAnimDecoderGetInfo(anim->decoder, &anim->info)) {
        unload_animation(anim);
        ESP_LOGE(TAG, "Failed to get animation info");
        return ESP_FAIL;
    }

    anim->direct_copy = (anim->info.canvas_width == FRAME_WIDTH && anim->info.canvas_height == FRAME_HEIGHT);

    if (!anim->direct_copy) {
        anim->x_map = (int *)malloc(FRAME_WIDTH * sizeof(int));
        anim->y_map = (int *)malloc(FRAME_HEIGHT * sizeof(int));
        if (!anim->x_map || !anim->y_map) {
            unload_animation(anim);
            ESP_LOGE(TAG, "Failed to allocate scaling indices");
            return ESP_ERR_NO_MEM;
        }
        const uint32_t x_step = ((uint32_t)anim->info.canvas_width << 16) / FRAME_WIDTH;
        const uint32_t y_step = ((uint32_t)anim->info.canvas_height << 16) / FRAME_HEIGHT;
        uint32_t acc = 0;
        for (int x = 0; x < FRAME_WIDTH; ++x) {
            int src = (int)(acc >> 16);
            if (src >= (int)anim->info.canvas_width) {
                src = (int)anim->info.canvas_width - 1;
            }
            anim->x_map[x] = src;
            acc += x_step;
        }
        acc = 0;
        for (int y = 0; y < FRAME_HEIGHT; ++y) {
            int src = (int)(acc >> 16);
            if (src >= (int)anim->info.canvas_height) {
                src = (int)anim->info.canvas_height - 1;
            }
            anim->y_map[y] = src;
            acc += y_step;
        }
    }

    ESP_LOGI(TAG, "Loaded animation %s (%u x %u, %u frames)",
             path,
             (unsigned)anim->info.canvas_width,
             (unsigned)anim->info.canvas_height,
             anim->info.frame_count);
    return ESP_OK;
}

static void unload_animation(playback_animation_t *anim)
{
    if (anim->is_gif) {
        // GIF files are handled by video_player, nothing to unload here
        anim->is_gif = false;
        return;
    }

    if (anim->decoder) {
        WebPAnimDecoderDelete(anim->decoder);
        anim->decoder = NULL;
    }
    free(anim->file_data);
    anim->file_data = NULL;
    free(anim->x_map);
    anim->x_map = NULL;
    free(anim->y_map);
    anim->y_map = NULL;
    memset(&anim->info, 0, sizeof(anim->info));
}

static esp_err_t render_next_frame(playback_engine_ctx_t *ctx)
{
    if (ctx->current.is_gif) {
        // GIF files are handled by video_player
        return ESP_OK;
    }

    uint8_t *frame_rgba = NULL;
    int timestamp_ms = 0;

    if (!ctx->current.decoder) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!WebPAnimDecoderGetNext(ctx->current.decoder, &frame_rgba, &timestamp_ms)) {
        WebPAnimDecoderReset(ctx->current.decoder);
        if (!WebPAnimDecoderGetNext(ctx->current.decoder, &frame_rgba, &timestamp_ms)) {
            return ESP_FAIL;
        }
    }

    if (!frame_rgba) {
        return ESP_FAIL;
    }

    uint8_t *dst = ctx->frame_buffer;
    
    if (ctx->current.direct_copy) {
        const size_t src_row_bytes = (size_t)ctx->current.info.canvas_width * 4;
        const size_t dst_row_bytes = (size_t)FRAME_WIDTH * 3;
        for (int y = 0; y < FRAME_HEIGHT; ++y) {
            const uint8_t *src_row = frame_rgba + (size_t)y * src_row_bytes;
            uint8_t *dst_row = dst + (size_t)y * dst_row_bytes;
            for (int x = 0; x < FRAME_WIDTH; ++x) {
                const uint8_t *src_px = src_row + (size_t)x * 4;
                uint8_t *dst_px = dst_row + (size_t)x * 3;
                // WebP outputs RGBA, but display expects BGR888 (swap R and B)
                dst_px[0] = src_px[2];  // B
                dst_px[1] = src_px[1];  // G
                dst_px[2] = src_px[0];  // R
            }
        }
    } else {
        // Ensure we fill the entire destination buffer
        for (int y = 0; y < FRAME_HEIGHT; ++y) {
            int src_y = ctx->current.y_map[y];
            // Clamp source Y to valid range
            if (src_y < 0) src_y = 0;
            if (src_y >= (int)ctx->current.info.canvas_height) {
                src_y = (int)ctx->current.info.canvas_height - 1;
            }
            const uint8_t *src_row = frame_rgba + (size_t)src_y * ctx->current.info.canvas_width * 4;
            uint8_t *dst_row = dst + (size_t)y * FRAME_WIDTH * 3;
            for (int x = 0; x < FRAME_WIDTH; ++x) {
                int src_x = ctx->current.x_map[x];
                // Clamp source X to valid range
                if (src_x < 0) src_x = 0;
                if (src_x >= (int)ctx->current.info.canvas_width) {
                    src_x = (int)ctx->current.info.canvas_width - 1;
                }
                const uint8_t *px = src_row + (size_t)src_x * 4;
                uint8_t *dst_px = dst_row + (size_t)x * 3;
                // WebP outputs RGBA, but display expects BGR888 (swap R and B)
                dst_px[0] = px[2];  // B
                dst_px[1] = px[1];  // G
                dst_px[2] = px[0];  // R
            }
        }
    }

    // Memory barrier: ensure all writes complete before cache sync
    __sync_synchronize();

    if (ctx->frame_buffer_spiram) {
        // Ensure cache sync covers the entire buffer, aligned to cache line boundaries
        // We need to flush cache TO memory (DIR_C2M) so DMA can see our writes
        uintptr_t buffer_start = (uintptr_t)ctx->frame_buffer;
        size_t buffer_size_bytes = ctx->frame_buffer_bytes;
        
        uintptr_t aligned_start = buffer_start & ~(uintptr_t)0x3F;
        uintptr_t buffer_end = buffer_start + buffer_size_bytes;
        uintptr_t aligned_end = (buffer_end + 0x3F) & ~(uintptr_t)0x3F;
        size_t sync_size = aligned_end - aligned_start;
        
        // Flush cache to SPIRAM: write-back all dirty cache lines so DMA can read them
        esp_cache_msync((void *)aligned_start, sync_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        
        // Memory barrier after cache sync: ensure cache flush completes before DMA starts
        __sync_synchronize();
    }

    // Additional memory barrier before DMA: ensure all memory operations complete
    __sync_synchronize();

    esp_err_t ret = esp_lcd_panel_draw_bitmap(ctx->panel, 0, 0, FRAME_WIDTH, FRAME_HEIGHT, dst);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_draw_bitmap failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Wait for DMA transfer to complete before starting next frame render
    // This prevents blue flashes from DMA reading partially-written buffer data
    if (ctx->trans_sem) {
        // Clear any stale semaphore signals (like LVGL does)
        while (xSemaphoreTake(ctx->trans_sem, 0) == pdTRUE) {
            // drain stale semaphore
        }

        const TickType_t wait_ticks = pdMS_TO_TICKS(250);
        if (xSemaphoreTake(ctx->trans_sem, wait_ticks) != pdTRUE) {
            ESP_LOGW(TAG, "Timeout waiting for DMA transfer completion (> %d ms)",
                     (int)(wait_ticks * portTICK_PERIOD_MS));
            return ESP_ERR_TIMEOUT;
        }
    }

    return ESP_OK;
}
