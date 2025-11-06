#include "player_internal.h"
#include "player_decoder.h"

#include "sd_ring.h"
#include "gif_decoder.h"
#include "webp/demux.h"
#include "webp/decode.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "player_decoder";

extern player_ctx_t* player_get_ctx(void);

static esp_err_t decode_gif_frame(player_ctx_t* ctx)
{
    if (!ctx->decoder.gif_decoder) {
        ESP_LOGE(TAG, "GIF decoder is NULL");
        return ESP_ERR_INVALID_STATE;
    }

    // Feed watchdog before decode
    esp_task_wdt_reset();

    // Set draw context to write directly to native buffer
    gif_draw_context_t draw_ctx = {
        .decoder_state = ctx->decoder.gif_decoder,
        .stripe_buffer = NULL,
        .stripe_y = 0,
        .stripe_height = 0,
        .display_width = ctx->native_width,
        .display_height = ctx->native_height,
        .frame_buffer = ctx->nwrite,
        .frame_width = ctx->native_width,
        .frame_height = ctx->native_height
    };
    gif_decoder_set_draw_context(ctx->decoder.gif_decoder, &draw_ctx);

    // Decode frame
    int delay_ms = 0;
    bool frame_ok = gif_decoder_play_frame(ctx->decoder.gif_decoder, &delay_ms);
    
    if (!frame_ok) {
        // End of animation - reset and loop
        ESP_LOGD(TAG, "GIF animation ended, resetting decoder...");
        gif_decoder_reset(ctx->decoder.gif_decoder);
        // Clear buffer for fresh start
        memset(ctx->nwrite, 0, (size_t)ctx->native_width * ctx->native_height * 3);
        return ESP_ERR_NOT_FINISHED;
    }

    // Sync cache (PSRAM write-back) - cache handled automatically in ESP-IDF v5.x
    __sync_synchronize();

    return ESP_OK;
}

static esp_err_t decode_webp_frame(player_ctx_t* ctx)
{
    if (!ctx->decoder.webp_decoder) {
        ESP_LOGE(TAG, "WebP decoder is NULL");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t* frame_rgba = NULL;
    int timestamp_ms = 0;

    // Feed watchdog before long decode operation
    esp_task_wdt_reset();

    if (!WebPAnimDecoderGetNext(ctx->decoder.webp_decoder, &frame_rgba, &timestamp_ms)) {
        // End of animation - reset and loop
        ESP_LOGD(TAG, "WebP animation ended, resetting decoder...");
        WebPAnimDecoderReset(ctx->decoder.webp_decoder);
        if (!WebPAnimDecoderGetNext(ctx->decoder.webp_decoder, &frame_rgba, &timestamp_ms)) {
            ESP_LOGE(TAG, "Failed to get first frame after reset");
            return ESP_ERR_NOT_FINISHED;
        }
    }

    if (!frame_rgba) {
        ESP_LOGE(TAG, "WebP frame_rgba is NULL");
        return ESP_FAIL;
    }

    // Feed watchdog during conversion
    esp_task_wdt_reset();

    // Convert RGBA to RGB888 and copy to native buffer
    // WebP outputs RGBA, but we need RGB888
    uint8_t* dst = ctx->nwrite;
    const uint8_t* src = frame_rgba;
    
    int width = ctx->native_width;
    int height = ctx->native_height;

    for (int y = 0; y < height; y++) {
        int x = 0;
        for (; x <= width - 4; x += 4) {
            // Unroll 4 pixels per iteration to reduce loop overhead
            dst[0]  = src[2];   dst[1]  = src[1];   dst[2]  = src[0];
            dst[3]  = src[6];   dst[4]  = src[5];   dst[5]  = src[4];
            dst[6]  = src[10];  dst[7]  = src[9];   dst[8]  = src[8];
            dst[9]  = src[14];  dst[10] = src[13];  dst[11] = src[12];
            dst += 12;
            src += 16;
        }
        for (; x < width; x++) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst += 3;
            src += 4;
        }

        if ((y & 3) == 3) {
            taskYIELD();
            esp_task_wdt_reset();
        }
    }

    // Allow idle task to run and give Wi-Fi stack a chance
    vTaskDelay(1);
    esp_task_wdt_reset();
    {
        static TickType_t last_feed_log = 0;
        TickType_t feed_now = xTaskGetTickCount();
        if (last_feed_log == 0 || (feed_now - last_feed_log) > pdMS_TO_TICKS(1000)) {
            uint32_t delta_ms = (last_feed_log == 0) ? 0U
                                                    : (uint32_t)((feed_now - last_feed_log) * portTICK_PERIOD_MS);
            ESP_LOGD(TAG, "Decoder watchdog fed (delta=%lu ms)", (unsigned long)delta_ms);
            last_feed_log = feed_now;
        }
    }

    // Sync cache (PSRAM write-back) - cache handled automatically in ESP-IDF v5.x
    __sync_synchronize();

    return ESP_OK;
}

void decoder_task(void* arg)
{
    player_ctx_t* ctx = (player_ctx_t*)arg;
    
    if (!ctx) {
        ESP_LOGE(TAG, "Decoder task: NULL context!");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "=== Decoder task started (Core 0) ===");
    ESP_LOGI(TAG, "Context: %p, is_gif: %d, queues: dec2ren=%p, ren2dec=%p", 
             (void*)ctx, ctx->is_gif, (void*)ctx->dec2ren_q, (void*)ctx->ren2dec_q);

    if (!ctx->dec2ren_q || !ctx->ren2dec_q) {
        ESP_LOGE(TAG, "Decoder task: Queues are NULL (dec2ren=%p, ren2dec=%p)!", 
                 (void*)ctx->dec2ren_q, (void*)ctx->ren2dec_q);
        vTaskDelete(NULL);
        return;
    }

    if (!ctx->nwrite || !ctx->nread) {
        ESP_LOGE(TAG, "Decoder task: Buffers are NULL (nwrite=%p, nread=%p)!", 
                 (void*)ctx->nwrite, (void*)ctx->nread);
        vTaskDelete(NULL);
        return;
    }

    // Add decoder task to watchdog
    ESP_LOGI(TAG, "Adding decoder task to watchdog...");
    esp_err_t wdt_ret = esp_task_wdt_add(NULL);
    if (wdt_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to add decoder task to watchdog: %s", esp_err_to_name(wdt_ret));
    }

    uint32_t decode_count = 0;
    TickType_t last_yield = xTaskGetTickCount();
    TickType_t last_stack_log = xTaskGetTickCount();

    while (ctx->running) {
        TickType_t now = xTaskGetTickCount();
        
        // Log stack usage periodically (every 5 seconds)
        if (now - last_stack_log > pdMS_TO_TICKS(5000)) {
            UBaseType_t stack_high_water = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGD(TAG, "Decoder task stack high water mark: %u bytes", 
                     stack_high_water * sizeof(StackType_t));
            last_stack_log = now;
        }

        // Feed watchdog frequently (every frame or 20ms)
        if (decode_count % 1 == 0 || (now - last_yield) > pdMS_TO_TICKS(20)) {
            esp_task_wdt_reset();
        }

        // Yield frequently to allow idle task to run (every ~20ms)
        if (now - last_yield > pdMS_TO_TICKS(20)) {
            taskYIELD();
            last_yield = now;
        }

        // Decode next frame
        esp_err_t ret;
        if (ctx->is_gif) {
            ret = decode_gif_frame(ctx);
        } else {
            ret = decode_webp_frame(ctx);
        }

        if (ret == ESP_ERR_NOT_FINISHED) {
            // Animation ended, loop
            ESP_LOGD(TAG, "Animation ended, looping...");
            continue;
        } else if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Frame decode failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        decode_count++;

        // Swap buffers: nwrite becomes nread
        uint8_t* tmp = ctx->nread;
        ctx->nread = ctx->nwrite;
        ctx->nwrite = tmp;

        // Notify renderer that frame is ready (non-blocking)
        uint8_t* frame_ptr = ctx->nread;
        if (xQueueSend(ctx->dec2ren_q, &frame_ptr, 0) != pdTRUE) {
            // Renderer is lagging - drop frame (prefer frame drop over blocking)
            ESP_LOGD(TAG, "Renderer queue full, dropping frame");
        }

        // Check for back-pressure from renderer (non-blocking)
        uint8_t* dummy;
        while (xQueueReceive(ctx->ren2dec_q, &dummy, 0) == pdTRUE) {
            // Drain back-pressure queue
        }

        // Yield to allow renderer to catch up
        taskYIELD();
    }

    // Remove from watchdog before exiting
    esp_task_wdt_delete(NULL);

    ESP_LOGI(TAG, "Decoder task exiting (decoded %lu frames)", (unsigned long)decode_count);
    vTaskDelete(NULL);
}

esp_err_t start_decoder(const anim_desc_t* desc)
{
    player_ctx_t* ctx = player_get_ctx();
    
    ESP_LOGI(TAG, "=== Starting decoder ===");
    ESP_LOGI(TAG, "Context: %p, desc: %p, path: '%s', type: %d, size: %d", 
             (void*)ctx, (void*)desc, desc ? desc->path : "NULL", 
             desc ? desc->type : -1, desc ? desc->native_size_px : -1);

    if (!ctx) {
        ESP_LOGE(TAG, "Player context is NULL!");
        return ESP_ERR_INVALID_STATE;
    }

    if (!desc || !desc->path) {
        ESP_LOGE(TAG, "Invalid descriptor (desc=%p, path=%p)", 
                 (void*)desc, desc ? desc->path : NULL);
        return ESP_ERR_INVALID_ARG;
    }

    // Stop existing decoder if running (but preserve running flag - it's set by player_start)
    ESP_LOGI(TAG, "Stopping existing decoder if running...");
    bool was_running = ctx->running;
    stop_decoder();
    // Restore running flag if it was set (will be set again by player_start, but this prevents race)
    ctx->running = was_running;

    ctx->is_gif = (desc->type == FILE_GIF);
    ctx->webp_data_buffer = NULL;  // Initialize to NULL

    if (ctx->is_gif) {
        // Initialize GIF decoder
        ctx->decoder.gif_decoder = (gif_decoder_state_t*)calloc(1, sizeof(gif_decoder_state_t));
        if (!ctx->decoder.gif_decoder) {
            return ESP_ERR_NO_MEM;
        }

        esp_err_t ret = gif_decoder_init(ctx->decoder.gif_decoder);
        if (ret != ESP_OK) {
            free(ctx->decoder.gif_decoder);
            ctx->decoder.gif_decoder = NULL;
            return ret;
        }

        // Open file - need to adapt gif_decoder to use sd_ring
        // For now, use file path directly (will need custom file callbacks)
        ret = gif_decoder_open_file(ctx->decoder.gif_decoder, desc->path);
        if (ret != ESP_OK) {
            gif_decoder_close(ctx->decoder.gif_decoder);
            free(ctx->decoder.gif_decoder);
            ctx->decoder.gif_decoder = NULL;
            return ret;
        }

        gif_decoder_get_canvas_size(ctx->decoder.gif_decoder, &ctx->native_width, &ctx->native_height);
        if (ctx->native_width != desc->native_size_px || ctx->native_height != desc->native_size_px) {
            ESP_LOGW(TAG, "GIF canvas size (%dx%d) doesn't match descriptor (%dx%d)",
                     ctx->native_width, ctx->native_height, desc->native_size_px, desc->native_size_px);
        }
    } else {
        // Initialize WebP decoder
        // Read file header to get size
        ssize_t file_size = sd_ring_get_file_size();
        if (file_size < 0) {
            return ESP_ERR_NOT_FOUND;
        }

        // Read WebP data (for now, read entire file - TODO: stream)
        uint8_t* webp_data = (uint8_t*)malloc((size_t)file_size);
        if (!webp_data) {
            return ESP_ERR_NO_MEM;
        }

        ssize_t read = sd_ring_read_at(0, webp_data, (size_t)file_size);
        if (read != file_size) {
            free(webp_data);
            return ESP_FAIL;
        }

        WebPAnimDecoderOptions options;
        if (!WebPAnimDecoderOptionsInit(&options)) {
            free(webp_data);
            return ESP_FAIL;
        }
        options.color_mode = MODE_RGBA;
        options.use_threads = 0;

        WebPData webp_data_wrapped = {
            .bytes = webp_data,
            .size = (size_t)file_size,
        };

        ctx->decoder.webp_decoder = WebPAnimDecoderNew(&webp_data_wrapped, &options);
        if (!ctx->decoder.webp_decoder) {
            free(webp_data);
            return ESP_FAIL;
        }

        // Store webp_data pointer for cleanup
        ctx->webp_data_buffer = webp_data;

        WebPAnimInfo info;
        if (!WebPAnimDecoderGetInfo(ctx->decoder.webp_decoder, &info)) {
            WebPAnimDecoderDelete(ctx->decoder.webp_decoder);
            ctx->decoder.webp_decoder = NULL;
            free(webp_data);
            ctx->webp_data_buffer = NULL;
            return ESP_FAIL;
        }

        ctx->native_width = (int)info.canvas_width;
        ctx->native_height = (int)info.canvas_height;
        
        if (ctx->native_width != desc->native_size_px || ctx->native_height != desc->native_size_px) {
            ESP_LOGW(TAG, "WebP canvas size (%dx%d) doesn't match descriptor (%dx%d)",
                     ctx->native_width, ctx->native_height, desc->native_size_px, desc->native_size_px);
        }
    }

    // Create decoder task (Core 0)
    ESP_LOGI(TAG, "Creating decoder task (Core 0, stack=12288, priority=5)...");
    BaseType_t task_ret = xTaskCreatePinnedToCore(
            decoder_task,
            "player_decoder",
            12288,  // Increased from 8192
            ctx,
            5,  // Priority 5
            &ctx->decoder_task,
            0);  // Core 0
    
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create decoder task (ret=%d)", task_ret);
        if (ctx->is_gif) {
            gif_decoder_close(ctx->decoder.gif_decoder);
            free(ctx->decoder.gif_decoder);
            ctx->decoder.gif_decoder = NULL;
        } else {
            WebPAnimDecoderDelete(ctx->decoder.webp_decoder);
            ctx->decoder.webp_decoder = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Decoder task created: %p", (void*)ctx->decoder_task);
    
    // Log stack watermark after a short delay to allow task to initialize
    vTaskDelay(pdMS_TO_TICKS(100));
    UBaseType_t stack_high_water = uxTaskGetStackHighWaterMark(ctx->decoder_task);
    ESP_LOGI(TAG, "Decoder task stack high water mark: %u bytes", stack_high_water * sizeof(StackType_t));
    
    ESP_LOGI(TAG, "=== Decoder started successfully: %s (%dx%d) ===", 
             desc->path, ctx->native_width, ctx->native_height);
    return ESP_OK;
}

void stop_decoder(void)
{
    player_ctx_t* ctx = player_get_ctx();

    if (ctx->decoder_task) {
        // Set running to false first to signal task to exit
        ctx->running = false;
        
        // Wait for task to exit gracefully (check if task handle is still valid)
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
        TaskHandle_t task_handle = ctx->decoder_task;
        
        while (task_handle != NULL && xTaskGetTickCount() < deadline) {
            // Check if task is still running by verifying handle
            if (eTaskGetState(task_handle) == eDeleted) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        
        // Force delete if still running
        if (task_handle != NULL && eTaskGetState(task_handle) != eDeleted) {
            ESP_LOGW(TAG, "Decoder task did not exit gracefully, force deleting");
            vTaskDelete(task_handle);
        }
        
        ctx->decoder_task = NULL;
    }

    if (ctx->is_gif && ctx->decoder.gif_decoder) {
        gif_decoder_close(ctx->decoder.gif_decoder);
        free(ctx->decoder.gif_decoder);
        ctx->decoder.gif_decoder = NULL;
    } else if (!ctx->is_gif && ctx->decoder.webp_decoder) {
        WebPAnimDecoderDelete(ctx->decoder.webp_decoder);
        ctx->decoder.webp_decoder = NULL;
        // Free WebP data buffer
        if (ctx->webp_data_buffer) {
            free(ctx->webp_data_buffer);
            ctx->webp_data_buffer = NULL;
        }
    }
}

