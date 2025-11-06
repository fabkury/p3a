#include "player_internal.h"
#include "player_renderer.h"
#include "player_decoder.h"

#include "scaler_nn.h"
#include "esp_lcd_panel_ops.h"

#include <string.h>
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "player_renderer";

#define HSTRIP 16
#define DST_WIDTH 720
#define DST_HEIGHT 720

extern player_ctx_t* player_get_ctx();

void renderer_task(void* arg)
{
    player_ctx_t* ctx = (player_ctx_t*)arg;
    
    if (!ctx) {
        ESP_LOGE(TAG, "Renderer task: NULL context!");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "=== Renderer task started (Core 1) ===");
    ESP_LOGI(TAG, "Context: %p, panel: %p, queues: dec2ren=%p, ren2dec=%p", 
             (void*)ctx, (void*)ctx->panel, (void*)ctx->dec2ren_q, (void*)ctx->ren2dec_q);

    if (!ctx->panel) {
        ESP_LOGE(TAG, "Renderer task: Panel handle is NULL!");
        vTaskDelete(NULL);
        return;
    }

    if (!ctx->dec2ren_q || !ctx->ren2dec_q) {
        ESP_LOGE(TAG, "Renderer task: Queues are NULL (dec2ren=%p, ren2dec=%p)!", 
                 (void*)ctx->dec2ren_q, (void*)ctx->ren2dec_q);
        vTaskDelete(NULL);
        return;
    }

    // Add renderer task to watchdog
    ESP_LOGI(TAG, "Adding renderer task to watchdog...");
    esp_err_t wdt_ret = esp_task_wdt_add(NULL);
    if (wdt_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to add renderer task to watchdog: %s", esp_err_to_name(wdt_ret));
    }

    uint8_t* last_frame = NULL;  // For frame repeat when decoder lags
    uint32_t frame_count = 0;
    uint32_t error_count = 0;
    TickType_t last_wdt_feed = xTaskGetTickCount();
    TickType_t last_stack_log = xTaskGetTickCount();

    // Calculate buffer bounds for validation
    uint8_t* native_buf_start = ctx->native_buf1 < ctx->native_buf2 ? ctx->native_buf1 : ctx->native_buf2;
    uint8_t* native_buf_end = ctx->native_buf1 > ctx->native_buf2 ? ctx->native_buf1 : ctx->native_buf2;
    size_t native_buf_size = (size_t)ctx->native_width * ctx->native_height * 3;
    native_buf_end += native_buf_size;

    while (ctx->running) {
        uint8_t* frame_ptr = NULL;
        BaseType_t got_frame = xQueueReceive(ctx->dec2ren_q, &frame_ptr, pdMS_TO_TICKS(100));

        if (got_frame != pdTRUE) {
            // No new frame - repeat last frame if available
            if (last_frame) {
                frame_ptr = last_frame;
            } else {
                // No frame at all yet - wait
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
        } else {
            // New frame available
            last_frame = frame_ptr;
        }

        // Validate frame pointer is within valid buffer range
        if (frame_ptr < native_buf_start || frame_ptr >= native_buf_end) {
            ESP_LOGE(TAG, "Invalid frame pointer: %p (valid range: %p-%p)", 
                     (void*)frame_ptr, (void*)native_buf_start, (void*)native_buf_end);
            error_count++;
            if (error_count > 5) {
                ESP_LOGE(TAG, "Too many errors, delaying before retry");
                vTaskDelay(pdMS_TO_TICKS(100));
                error_count = 0;
            }
            continue;
        }

        // Render frame strip by strip
        bool use_ping = true;
        bool frame_complete = true;

        for (int y = 0; y < DST_HEIGHT && ctx->running; y += HSTRIP) {
            int strip_height = (DST_HEIGHT - y < HSTRIP) ? (DST_HEIGHT - y) : HSTRIP;
            uint8_t* strip_buf = use_ping ? ctx->strip_ping : ctx->strip_pong;

            // Feed watchdog before long operations
            esp_task_wdt_reset();

            // Sync cache for native buffer read (PSRAM)
            size_t native_buf_size = (size_t)ctx->native_width * ctx->native_height * 3;
            // Explicitly invalidate cache to ensure fresh PSRAM data
            esp_cache_msync(frame_ptr, native_buf_size, ESP_CACHE_MSYNC_FLAG_INVALIDATE);
            __sync_synchronize();

            // Upscale strip from native buffer
            for (int r = 0; r < strip_height; r++) {
                int y_out = y + r;
                
                // Compute source Y using nearest-neighbor
                // y_map: floor((y_out + 0.5) * native_height / DST_HEIGHT)
                int src_y = (int)(((int64_t)y_out * ctx->native_height + DST_HEIGHT / 2) / DST_HEIGHT);
                if (src_y < 0) src_y = 0;
                if (src_y >= ctx->native_height) src_y = ctx->native_height - 1;

                const uint8_t* src_row = frame_ptr + (size_t)src_y * ctx->native_width * 3;
                uint8_t* dst_row = strip_buf + (size_t)r * DST_WIDTH * 3;

                // Scale row using precomputed map
                nn_scale_row_rgb888(src_row, ctx->native_width, ctx->scaler_map, dst_row);
                
                // Yield every 4 rows to keep watchdog happy
                if (r % 4 == 0) {
                    taskYIELD();
                    esp_task_wdt_reset();
                }
            }

                    // Sync cache for strip buffer (SRAM, ensure DMA can read it)
            size_t strip_size = (size_t)DST_WIDTH * strip_height * 3;
            // Explicitly writeback cache to ensure DMA can read SRAM data
            // Use C2M (Cache-to-Memory) to flush our writes to memory for DMA
            // Align to cache line boundary (required for C2M)
            uintptr_t aligned_start = (uintptr_t)strip_buf & ~(uintptr_t)0x3F;
            uintptr_t aligned_end = ((uintptr_t)strip_buf + strip_size + 0x3F) & ~(uintptr_t)0x3F;
            size_t aligned_size = aligned_end - aligned_start;
            esp_cache_msync((void*)aligned_start, aligned_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            __sync_synchronize();

            // Validate strip buffer pointer
            if (!strip_buf || (strip_buf != ctx->strip_ping && strip_buf != ctx->strip_pong)) {
                ESP_LOGE(TAG, "Invalid strip buffer pointer: %p", (void*)strip_buf);
                error_count++;
                frame_complete = false;
                break;
            }

            // Check panel validity before DMA operation
            if (!ctx->panel || !ctx->running) {
                ESP_LOGW(TAG, "Panel invalid or player stopped, exiting render loop");
                frame_complete = false;
                break;
            }

                    SemaphoreHandle_t trans_sem = ctx->trans_sem;
                    if (trans_sem) {
                        xSemaphoreTake(trans_sem, 0);
                    }

                    // Send strip via DMA
                    // Note: For DSI panels, draw_bitmap may be blocking or non-blocking depending on configuration
                    // We wait for completion via semaphore to ensure tear-free rendering
                    esp_err_t ret = esp_lcd_panel_draw_bitmap(ctx->panel, 0, y, DST_WIDTH, y + strip_height, strip_buf);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to draw bitmap strip: %s", esp_err_to_name(ret));
                error_count++;
                frame_complete = false;
                if (error_count > 5) {
                    ESP_LOGE(TAG, "Too many DMA errors, delaying before retry");
                    vTaskDelay(pdMS_TO_TICKS(100));
                    error_count = 0;
                }
                break;
            }

            // Wait for DMA transfer completion (with timeout)
            // This ensures tear-free rendering by waiting for each strip to complete
            // Feed watchdog before potentially blocking wait
            esp_task_wdt_reset();
            
                    if (trans_sem) {
                const TickType_t wait_ticks = pdMS_TO_TICKS(100);  // Reduced timeout to prevent blocking too long
                if (xSemaphoreTake(trans_sem, wait_ticks) != pdTRUE) {
                    ESP_LOGW(TAG, "Timeout waiting for DMA transfer completion (strip y=%d)", y);
                    // Don't break - continue with next strip to avoid complete frame loss
                    // This may cause minor tearing but prevents complete stall
                }
            } else {
                // No semaphore - assume blocking call or no completion notification needed
                // Feed watchdog since we may have blocked
                esp_task_wdt_reset();
            }

            // Allow other tasks (notably idle) to run between strips
            taskYIELD();
            esp_task_wdt_reset();

            // Switch ping-pong for next strip
            use_ping = !use_ping;
        }

        if (frame_complete) {
            // Reset error count on successful frame
            error_count = 0;
            
            // Notify decoder that we're done with this frame (back-pressure)
            // Non-blocking - prefer frame drop over blocking
            uint8_t* dummy = frame_ptr;
            xQueueSend(ctx->ren2dec_q, &dummy, 0);
        }

        frame_count++;

        // Feed watchdog frequently (every frame or 20ms)
        TickType_t now = xTaskGetTickCount();
        if (frame_count % 1 == 0 || (now - last_wdt_feed) > pdMS_TO_TICKS(20)) {
            esp_task_wdt_reset();
            if (last_wdt_feed == 0 || (now - last_wdt_feed) > pdMS_TO_TICKS(1000)) {
                uint32_t delta_ms = (last_wdt_feed == 0)
                                        ? 0U
                                        : (uint32_t)((now - last_wdt_feed) * portTICK_PERIOD_MS);
                ESP_LOGD(TAG, "Renderer watchdog fed (delta=%lu ms)", (unsigned long)delta_ms);
            }
            last_wdt_feed = now;
        }

        // Log stack usage periodically (every 5 seconds)
        if (now - last_stack_log > pdMS_TO_TICKS(5000)) {
            UBaseType_t stack_high_water = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGD(TAG, "Renderer task stack high water mark: %u bytes", 
                     stack_high_water * sizeof(StackType_t));
            last_stack_log = now;
        }

        // Allow other tasks (including IDLE) to run
        vTaskDelay(1);
    }

    // Remove from watchdog before exiting
    esp_task_wdt_delete(NULL);

    ESP_LOGI(TAG, "Renderer task exiting (rendered %lu frames)", (unsigned long)frame_count);
    vTaskDelete(NULL);
}

static esp_err_t start_renderer(void)
{
    player_ctx_t* ctx = player_get_ctx();
    
    ESP_LOGI(TAG, "=== Starting renderer ===");
    ESP_LOGI(TAG, "Context: %p, panel: %p, trans_sem: %p", 
             (void*)ctx, ctx ? (void*)ctx->panel : NULL, ctx ? (void*)ctx->trans_sem : NULL);

    if (!ctx) {
        ESP_LOGE(TAG, "Player context is NULL!");
        return ESP_ERR_INVALID_STATE;
    }

    if (!ctx->panel) {
        ESP_LOGE(TAG, "Panel handle is NULL!");
        return ESP_ERR_INVALID_STATE;
    }

    if (ctx->renderer_task) {
        ESP_LOGW(TAG, "Renderer task already running");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Creating renderer task (Core 1, stack=12288, priority=7)...");
    // Create renderer task (Core 1)
    BaseType_t ret = xTaskCreatePinnedToCore(
            renderer_task,
            "player_renderer",
            12288,  // Increased from 8192
            ctx,
            7,  // Priority 7 (higher than decoder)
            &ctx->renderer_task,
            1);  // Core 1
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create renderer task (ret=%d)", ret);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Renderer task created: %p", (void*)ctx->renderer_task);
    
    // Log stack watermark after a short delay to allow task to initialize
    vTaskDelay(pdMS_TO_TICKS(100));
    UBaseType_t stack_high_water = uxTaskGetStackHighWaterMark(ctx->renderer_task);
    ESP_LOGI(TAG, "Renderer task stack high water mark: %u bytes", stack_high_water * sizeof(StackType_t));
    
    ESP_LOGI(TAG, "=== Renderer started successfully ===");
    return ESP_OK;
}

static void stop_renderer(void)
{
    player_ctx_t* ctx = player_get_ctx();

    if (ctx->renderer_task) {
        // Set running to false first to signal task to exit
        ctx->running = false;
        
        // Wait for task to exit gracefully (check if task handle is still valid)
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
        TaskHandle_t task_handle = ctx->renderer_task;
        
        while (task_handle != NULL && xTaskGetTickCount() < deadline) {
            // Check if task is still running by verifying handle
            if (eTaskGetState(task_handle) == eDeleted) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        
        // Force delete if still running
        if (task_handle != NULL && eTaskGetState(task_handle) != eDeleted) {
            ESP_LOGW(TAG, "Renderer task did not exit gracefully, force deleting");
            vTaskDelete(task_handle);
        }
        
        ctx->renderer_task = NULL;
    }
}

// Exported functions
void player_renderer_start(void)
{
    start_renderer();
}

void player_renderer_stop(void)
{
    stop_renderer();
}

