#include "animation_player_priv.h"
#include "ugfx_ui.h"

#define DIGIT_WIDTH  5
#define DIGIT_HEIGHT 7

#if defined(CONFIG_P3A_LCD_DISPLAY_FRAME_DURATIONS)
static const uint8_t digit_font[10][DIGIT_HEIGHT] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x1F},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    {0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
};

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r & 0xF8) << 8) |
                      ((uint16_t)(g & 0xFC) << 3) |
                      ((uint16_t)b >> 3));
}

#if CONFIG_LCD_PIXEL_FORMAT_RGB565
typedef uint16_t app_lcd_color_t;

static inline app_lcd_color_t app_lcd_make_color(uint8_t r, uint8_t g, uint8_t b)
{
    return rgb565(r, g, b);
}

static inline void app_lcd_store_pixel(uint8_t *frame, int x, int y, app_lcd_color_t color)
{
    uint16_t *dst_row = (uint16_t *)(frame + (size_t)y * s_frame_row_stride_bytes);
    const size_t row_pixels = s_frame_row_stride_bytes / sizeof(uint16_t);
    if ((size_t)x >= row_pixels) {
        return;
    }
    dst_row[x] = color;
}
#elif CONFIG_LCD_PIXEL_FORMAT_RGB888
typedef uint32_t app_lcd_color_t;

static inline app_lcd_color_t app_lcd_make_color(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline void app_lcd_store_pixel(uint8_t *frame, int x, int y, app_lcd_color_t color)
{
    uint8_t *row = frame + (size_t)y * s_frame_row_stride_bytes;
    const size_t idx = (size_t)x * 3U;
    if ((idx + 2) >= s_frame_row_stride_bytes) {
        return;
    }
    row[idx + 0] = (uint8_t)color;
    row[idx + 1] = (uint8_t)(color >> 8);
    row[idx + 2] = (uint8_t)(color >> 16);
}
#else
#error "Unsupported LCD pixel format"
#endif

static inline void draw_pixel(uint8_t *frame, int x, int y, app_lcd_color_t color)
{
    if (!frame) {
        return;
    }
    if (x < 0 || x >= EXAMPLE_LCD_H_RES || y < 0 || y >= EXAMPLE_LCD_V_RES) {
        return;
    }
    app_lcd_store_pixel(frame, x, y, color);
}

static int char_pixel_width(char c, int scale)
{
    if (scale <= 0) {
        scale = 1;
    }
    if (c >= '0' && c <= '9') {
        return (DIGIT_WIDTH * scale) + scale;
    }
    if (c == '.' || c == ',') {
        return scale * 2;
    }
    if (c == '-') {
        return (DIGIT_WIDTH * scale) + scale;
    }
    return scale * 3;
}

static void draw_char(uint8_t *frame, char c, int x, int y, int scale, app_lcd_color_t color)
{
    if (!frame) {
        return;
    }
    if (scale <= 0) {
        scale = 1;
    }
    if (c == ' ') {
        return;
    }
    if (c == '.' || c == ',') {
        const int dot_size = MAX(1, scale / 2);
        const int base_x = x;
        const int base_y = y + (DIGIT_HEIGHT * scale) - dot_size - 1;
        for (int dy = 0; dy < dot_size; ++dy) {
            for (int dx = 0; dx < dot_size; ++dx) {
                draw_pixel(frame, base_x + dx, base_y + dy, color);
            }
        }
        return;
    }
    if (c == '-') {
        const int line_height = MAX(1, scale / 2);
        const int base_y = y + (DIGIT_HEIGHT * scale) / 2;
        for (int dy = 0; dy < line_height; ++dy) {
            for (int dx = 0; dx < DIGIT_WIDTH * scale; ++dx) {
                draw_pixel(frame, x + dx, base_y + dy, color);
            }
        }
        return;
    }
    if (c < '0' || c > '9') {
        return;
    }

    const uint8_t *glyph = digit_font[c - '0'];
    for (int row = 0; row < DIGIT_HEIGHT; ++row) {
        const uint8_t bits = glyph[row];
        for (int col = 0; col < DIGIT_WIDTH; ++col) {
            if ((bits >> (DIGIT_WIDTH - 1 - col)) & 0x01) {
                const int px = x + col * scale;
                const int py = y + row * scale;
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale; ++dx) {
                        draw_pixel(frame, px + dx, py + dy, color);
                    }
                }
            }
        }
    }
}

static void draw_text(uint8_t *frame, const char *text, int x, int y, int scale, app_lcd_color_t color)
{
    if (!frame || !text) {
        return;
    }
    if (scale <= 0) {
        scale = 1;
    }
    int cursor_x = x;
    for (const char *ch = text; *ch; ++ch) {
        draw_char(frame, *ch, cursor_x, y, scale, color);
        cursor_x += char_pixel_width(*ch, scale);
    }
}

static int measure_text_width(const char *text, int scale)
{
    if (!text || scale <= 0) {
        return 0;
    }
    int width = 0;
    for (const char *ch = text; *ch; ++ch) {
        width += char_pixel_width(*ch, scale);
    }
    return width;
}

static void draw_text_top_right(uint8_t *frame, const char *text, int margin_x, int margin_y, int scale, app_lcd_color_t color)
{
    if (!frame || !text) {
        return;
    }

    if (scale <= 0) {
        scale = 1;
    }
    const int width = measure_text_width(text, scale);
    int draw_x = EXAMPLE_LCD_H_RES - margin_x - width;
    if (draw_x < 0) {
        draw_x = 0;
    }
    draw_text(frame, text, draw_x, margin_y, scale, color);
}
#endif // CONFIG_P3A_LCD_DISPLAY_FRAME_DURATIONS

static void blit_webp_frame_rows(const uint8_t *src_rgba, int src_w, int src_h,
                                 uint8_t *dst_buffer, int dst_w, int dst_h,
                                 int row_start, int row_end,
                                 const uint16_t *lookup_x, const uint16_t *lookup_y)
{
    if (!src_rgba || !dst_buffer || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return;
    }

    if (row_start < 0) row_start = 0;
    if (row_end > dst_h) row_end = dst_h;
    if (row_start >= row_end) return;

    if (!lookup_x || !lookup_y) {
        ESP_LOGE(TAG, "Upscale lookup tables not initialized");
        return;
    }

    const screen_rotation_t rotation = s_upscale_rotation;

    for (int dst_y = row_start; dst_y < row_end; ++dst_y) {
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
        uint16_t *dst_row = (uint16_t *)(dst_buffer + (size_t)dst_y * s_frame_row_stride_bytes);
#else
        uint8_t *dst_row = dst_buffer + (size_t)dst_y * s_frame_row_stride_bytes;
#endif

        switch (rotation) {
            case ROTATION_0: {
                // Standard: src(lookup_x[dst_x], lookup_y[dst_y])
                const uint16_t src_y = lookup_y[dst_y];
                const uint8_t *src_row = src_rgba + (size_t)src_y * src_w * 4;
                for (int dst_x = 0; dst_x < dst_w; ++dst_x) {
                    const uint16_t src_x = lookup_x[dst_x];
                    const uint8_t *pixel = src_row + (size_t)src_x * 4;
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                    dst_row[dst_x] = rgb565(pixel[0], pixel[1], pixel[2]);
#else
                    const size_t idx = (size_t)dst_x * 3U;
                    if ((idx + 2) < s_frame_row_stride_bytes) {
                        dst_row[idx + 0] = pixel[2];
                        dst_row[idx + 1] = pixel[1];
                        dst_row[idx + 2] = pixel[0];
                    }
#endif
                }
                break;
            }
            
            case ROTATION_90: {
                // 90° CW: src(lookup_x[dst_y], src_h - 1 - lookup_y[dst_x])
                // For each dst pixel, src_x = scaled(dst_y), src_y = src_h - 1 - scaled(dst_x)
                const uint16_t src_x_fixed = lookup_x[dst_y];  // src_x constant for this dst row
                for (int dst_x = 0; dst_x < dst_w; ++dst_x) {
                    const uint16_t raw_src_y = lookup_y[dst_x];
                    const uint16_t src_y = (src_h - 1) - raw_src_y;
                    const uint8_t *pixel = src_rgba + ((size_t)src_y * src_w + src_x_fixed) * 4;
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                    dst_row[dst_x] = rgb565(pixel[0], pixel[1], pixel[2]);
#else
                    const size_t idx = (size_t)dst_x * 3U;
                    if ((idx + 2) < s_frame_row_stride_bytes) {
                        dst_row[idx + 0] = pixel[2];
                        dst_row[idx + 1] = pixel[1];
                        dst_row[idx + 2] = pixel[0];
                    }
#endif
                }
                break;
            }
            
            case ROTATION_180: {
                // 180°: src(src_w - 1 - lookup_x[dst_x], src_h - 1 - lookup_y[dst_y])
                const uint16_t raw_src_y = lookup_y[dst_y];
                const uint16_t src_y = (src_h - 1) - raw_src_y;
                const uint8_t *src_row = src_rgba + (size_t)src_y * src_w * 4;
                for (int dst_x = 0; dst_x < dst_w; ++dst_x) {
                    const uint16_t raw_src_x = lookup_x[dst_x];
                    const uint16_t src_x = (src_w - 1) - raw_src_x;
                    const uint8_t *pixel = src_row + (size_t)src_x * 4;
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                    dst_row[dst_x] = rgb565(pixel[0], pixel[1], pixel[2]);
#else
                    const size_t idx = (size_t)dst_x * 3U;
                    if ((idx + 2) < s_frame_row_stride_bytes) {
                        dst_row[idx + 0] = pixel[2];
                        dst_row[idx + 1] = pixel[1];
                        dst_row[idx + 2] = pixel[0];
                    }
#endif
                }
                break;
            }
            
            case ROTATION_270: {
                // 270° CW (90° CCW): src(src_w - 1 - lookup_x[dst_y], lookup_y[dst_x])
                // For each dst pixel, src_x = src_w - 1 - scaled(dst_y), src_y = scaled(dst_x)
                const uint16_t raw_src_x = lookup_x[dst_y];
                const uint16_t src_x_fixed = (src_w - 1) - raw_src_x;
                for (int dst_x = 0; dst_x < dst_w; ++dst_x) {
                    const uint16_t src_y = lookup_y[dst_x];
                    const uint8_t *pixel = src_rgba + ((size_t)src_y * src_w + src_x_fixed) * 4;
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                    dst_row[dst_x] = rgb565(pixel[0], pixel[1], pixel[2]);
#else
                    const size_t idx = (size_t)dst_x * 3U;
                    if ((idx + 2) < s_frame_row_stride_bytes) {
                        dst_row[idx + 0] = pixel[2];
                        dst_row[idx + 1] = pixel[1];
                        dst_row[idx + 2] = pixel[0];
                    }
#endif
                }
                break;
            }
            
            default: {
                // Fallback to no rotation
                const uint16_t src_y = lookup_y[dst_y];
                const uint8_t *src_row = src_rgba + (size_t)src_y * src_w * 4;
                for (int dst_x = 0; dst_x < dst_w; ++dst_x) {
                    const uint16_t src_x = lookup_x[dst_x];
                    const uint8_t *pixel = src_row + (size_t)src_x * 4;
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
                    dst_row[dst_x] = rgb565(pixel[0], pixel[1], pixel[2]);
#else
                    const size_t idx = (size_t)dst_x * 3U;
                    if ((idx + 2) < s_frame_row_stride_bytes) {
                        dst_row[idx + 0] = pixel[2];
                        dst_row[idx + 1] = pixel[1];
                        dst_row[idx + 2] = pixel[0];
                    }
#endif
                }
                break;
            }
        }
    }
}

void upscale_worker_top_task(void *arg)
{
    (void)arg;
    const uint32_t notification_bit = (1UL << 0);

    while (true) {
        uint32_t notification_value = 0;
        xTaskNotifyWait(0, UINT32_MAX, &notification_value, portMAX_DELAY);

        MEMORY_BARRIER();

        if (s_upscale_src_buffer && s_upscale_dst_buffer &&
            s_upscale_row_start_top < s_upscale_row_end_top) {
            blit_webp_frame_rows(s_upscale_src_buffer,
                                 s_upscale_src_w, s_upscale_src_h,
                                 s_upscale_dst_buffer,
                                 EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES,
                                 s_upscale_row_start_top, s_upscale_row_end_top,
                                 s_upscale_lookup_x, s_upscale_lookup_y);
        }

        MEMORY_BARRIER();

        s_upscale_worker_top_done = true;
        if (s_upscale_main_task) {
            xTaskNotify(s_upscale_main_task, notification_bit, eSetBits);
        }
    }
}

void upscale_worker_bottom_task(void *arg)
{
    (void)arg;
    const uint32_t notification_bit = (1UL << 1);

    while (true) {
        uint32_t notification_value = 0;
        xTaskNotifyWait(0, UINT32_MAX, &notification_value, portMAX_DELAY);

        MEMORY_BARRIER();

        if (s_upscale_src_buffer && s_upscale_dst_buffer &&
            s_upscale_row_start_bottom < s_upscale_row_end_bottom) {
            blit_webp_frame_rows(s_upscale_src_buffer,
                                 s_upscale_src_w, s_upscale_src_h,
                                 s_upscale_dst_buffer,
                                 EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES,
                                 s_upscale_row_start_bottom, s_upscale_row_end_bottom,
                                 s_upscale_lookup_x, s_upscale_lookup_y);
        }

        MEMORY_BARRIER();

        s_upscale_worker_bottom_done = true;
        if (s_upscale_main_task) {
            xTaskNotify(s_upscale_main_task, notification_bit, eSetBits);
        }
    }
}

static int render_next_frame(animation_buffer_t *buf, uint8_t *dest_buffer, int target_w, int target_h, bool use_prefetched)
{
    // Note: Caller should hold s_buffer_mutex and verify UI mode is not active
    if (!buf || !buf->ready || !dest_buffer || !buf->decoder) {
        return -1;
    }

    if (use_prefetched && buf->first_frame_ready && buf->prefetched_first_frame) {
        memcpy(dest_buffer, buf->prefetched_first_frame, s_frame_buffer_bytes);
        buf->first_frame_ready = false;
        return (int)buf->prefetched_first_frame_delay_ms;
    }

    if (!buf->native_frame_b1 || !buf->native_frame_b2) {
        ESP_LOGE(TAG, "Native frame buffers not allocated");
        return -1;
    }

    uint8_t *decode_buffer = (buf->native_buffer_active == 0) ? buf->native_frame_b1 : buf->native_frame_b2;

    esp_err_t err = animation_decoder_decode_next(buf->decoder, decode_buffer);
    if (err == ESP_ERR_INVALID_STATE) {
        animation_decoder_reset(buf->decoder);
        err = animation_decoder_decode_next(buf->decoder, decode_buffer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Animation decoder could not restart");
            return -1;
        }
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to decode frame: %s", esp_err_to_name(err));
        return -1;
    }

    uint32_t frame_delay_ms = 1;
    esp_err_t delay_err = animation_decoder_get_frame_delay(buf->decoder, &frame_delay_ms);
    if (delay_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get frame delay, using default");
        frame_delay_ms = 1;
    }
    buf->current_frame_delay_ms = frame_delay_ms;

    buf->native_buffer_active = (buf->native_buffer_active == 0) ? 1 : 0;

    const uint8_t *src_for_upscale = decode_buffer;

    s_upscale_src_buffer = src_for_upscale;
    s_upscale_dst_buffer = dest_buffer;
    s_upscale_lookup_x = buf->upscale_lookup_x;
    s_upscale_lookup_y = buf->upscale_lookup_y;
    s_upscale_src_w = buf->upscale_src_w;
    s_upscale_src_h = buf->upscale_src_h;
    s_upscale_rotation = g_screen_rotation;
    s_upscale_main_task = xTaskGetCurrentTaskHandle();

    const int dst_h = target_h;
    const int mid_row = dst_h / 2;

    s_upscale_worker_top_done = false;
    s_upscale_worker_bottom_done = false;

    s_upscale_row_start_top = 0;
    s_upscale_row_end_top = mid_row;
    s_upscale_row_start_bottom = mid_row;
    s_upscale_row_end_bottom = dst_h;

    MEMORY_BARRIER();

    if (s_upscale_worker_top && s_upscale_worker_bottom) {
        xTaskNotify(s_upscale_worker_top, 1, eSetBits);
        xTaskNotify(s_upscale_worker_bottom, 1, eSetBits);
    }

    const uint32_t all_bits = (1UL << 0) | (1UL << 1);
    uint32_t notification_value = 0;

    while ((notification_value & all_bits) != all_bits) {
        uint32_t received_bits = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &received_bits, pdMS_TO_TICKS(50)) == pdTRUE) {
            notification_value |= received_bits;
        } else {
            taskYIELD();
        }
    }

    if (!s_upscale_worker_top_done || !s_upscale_worker_bottom_done) {
        ESP_LOGW(TAG, "Upscale workers may not have completed properly");
    }

    MEMORY_BARRIER();

    return (int)buf->current_frame_delay_ms;
}

bool lcd_panel_refresh_done_cb(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    (void)panel;
    (void)edata;
    SemaphoreHandle_t sem = (SemaphoreHandle_t)user_ctx;
    BaseType_t higher_prio_task_woken = pdFALSE;
    if (sem) {
        xSemaphoreGiveFromISR(sem, &higher_prio_task_woken);
    }
    return higher_prio_task_woken == pdTRUE;
}


esp_err_t prefetch_first_frame(animation_buffer_t *buf)
{
    if (!buf || !buf->decoder || !buf->prefetched_first_frame) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *decode_buffer = buf->native_frame_b1;
    esp_err_t err = animation_decoder_decode_next(buf->decoder, decode_buffer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to decode first frame for prefetch: %s", esp_err_to_name(err));
        return err;
    }

    uint32_t frame_delay_ms = 1;
    esp_err_t delay_err = animation_decoder_get_frame_delay(buf->decoder, &frame_delay_ms);
    if (delay_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get prefetch frame delay, using default");
        frame_delay_ms = 1;
    }
    buf->prefetched_first_frame_delay_ms = frame_delay_ms;

    const uint8_t *src_for_upscale = decode_buffer;
    const int target_h = EXAMPLE_LCD_V_RES;
    const int dst_h = target_h;
    const int mid_row = dst_h / 2;

    s_upscale_src_buffer = src_for_upscale;
    s_upscale_dst_buffer = buf->prefetched_first_frame;
    s_upscale_lookup_x = buf->upscale_lookup_x;
    s_upscale_lookup_y = buf->upscale_lookup_y;
    s_upscale_src_w = buf->upscale_src_w;
    s_upscale_src_h = buf->upscale_src_h;
    s_upscale_rotation = g_screen_rotation;
    s_upscale_main_task = xTaskGetCurrentTaskHandle();

    s_upscale_worker_top_done = false;
    s_upscale_worker_bottom_done = false;

    s_upscale_row_start_top = 0;
    s_upscale_row_end_top = mid_row;
    s_upscale_row_start_bottom = mid_row;
    s_upscale_row_end_bottom = dst_h;

    if (!s_upscale_worker_top || !s_upscale_worker_bottom) {
        ESP_LOGE(TAG, "Upscale workers not available for prefetch");
        return ESP_ERR_INVALID_STATE;
    }

    MEMORY_BARRIER();

    xTaskNotify(s_upscale_worker_top, 1, eSetBits);
    xTaskNotify(s_upscale_worker_bottom, 1, eSetBits);

    const uint32_t all_bits = (1UL << 0) | (1UL << 1);
    uint32_t notification_value = 0;

    while ((notification_value & all_bits) != all_bits) {
        uint32_t received_bits = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &received_bits, pdMS_TO_TICKS(50)) == pdTRUE) {
            notification_value |= received_bits;
        } else {
            taskYIELD();
        }
    }

    if (!s_upscale_worker_top_done || !s_upscale_worker_bottom_done) {
        ESP_LOGW(TAG, "Upscale workers may not have completed properly during prefetch");
        return ESP_FAIL;
    }

    MEMORY_BARRIER();

    buf->first_frame_ready = true;
    buf->decoder_at_frame_1 = true;

    ESP_LOGD(TAG, "Prefetched first frame for animation index %zu", buf->asset_index);

    return ESP_OK;
}

void lcd_animation_task(void *arg)
{
    (void)arg;
#if defined(CONFIG_P3A_LCD_DISPLAY_FRAME_DURATIONS)
    const app_lcd_color_t color_red = app_lcd_make_color(0xFF, 0x20, 0x20);
    const app_lcd_color_t color_white = app_lcd_make_color(0xFF, 0xFF, 0xFF);
#endif

    const bool use_vsync = (s_buffer_count > 1) && (s_vsync_sem != NULL);
    const uint8_t buffer_count = (s_buffer_count == 0) ? 1 : s_buffer_count;
    bool use_prefetched = false;

    while (true) {
        // Wait for vsync if available
        if (use_vsync) {
            xSemaphoreTake(s_vsync_sem, portMAX_DELAY);
        }

        // Check render mode FIRST - request → active handshake
        const render_mode_t mode = s_render_mode_request;
        s_render_mode_active = mode;
        const bool ui_mode = (mode == RENDER_MODE_UI);

        // Get the back buffer
        uint8_t back_buffer_idx = s_render_buffer_index;
        uint8_t *back_buffer = s_lcd_buffers[back_buffer_idx];
        
        if (!back_buffer) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Record frame processing start time
        s_frame_processing_start_us = esp_timer_get_time();
        
        int frame_delay_ms = 100;
        uint32_t prev_frame_delay_ms = s_target_frame_delay_ms;

        // =====================================================================
        // UI MODE: Render UI directly to back buffer
        // =====================================================================
        if (ui_mode) {
            frame_delay_ms = ugfx_ui_render_to_buffer(back_buffer, s_frame_row_stride_bytes);
            if (frame_delay_ms < 0) {
                // UI not ready yet - show black screen
                memset(back_buffer, 0, s_frame_buffer_bytes);
                frame_delay_ms = 100;
            }
            s_target_frame_delay_ms = (uint32_t)frame_delay_ms;
        }
        // =====================================================================
        // ANIMATION MODE: Render animation frame
        // =====================================================================
        else {
            // Read animation state with mutex
            bool paused_local = false;
            bool swap_requested = false;
            bool back_buffer_ready = false;
            bool back_buffer_prefetch_pending = false;

            if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
                paused_local = s_anim_paused;
                swap_requested = s_swap_requested;
                back_buffer_ready = s_back_buffer.ready;
                back_buffer_prefetch_pending = s_back_buffer.prefetch_pending;
                xSemaphoreGive(s_buffer_mutex);
            }

            // Handle prefetch (outside mutex - this can take time)
            if (back_buffer_prefetch_pending) {
                esp_err_t prefetch_err = prefetch_first_frame(&s_back_buffer);
                if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
                    s_back_buffer.prefetch_pending = false;
                    s_back_buffer.ready = (prefetch_err == ESP_OK);
                    swap_requested = s_swap_requested;
                    back_buffer_ready = s_back_buffer.ready;
                    xSemaphoreGive(s_buffer_mutex);
                }
            }

            // Handle buffer swap
            if (swap_requested && back_buffer_ready) {
                if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
                    animation_buffer_t temp = s_front_buffer;
                    s_front_buffer = s_back_buffer;
                    s_back_buffer = temp;
                    s_swap_requested = false;
                    s_back_buffer.ready = false;
                    s_back_buffer.first_frame_ready = false;
                    s_back_buffer.prefetch_pending = false;
                    xSemaphoreGive(s_buffer_mutex);
                    ESP_LOGI(TAG, "Buffers swapped: front now playing index %zu", s_front_buffer.asset_index);
                }
                use_prefetched = true;
            }

            // Render frame
#if CONFIG_P3A_PICO8_ENABLE
            bool pico8_active = pico8_stream_should_render();
            if (pico8_active) {
                frame_delay_ms = render_pico8_frame(back_buffer);
                if (frame_delay_ms < 0) frame_delay_ms = 16;
                s_target_frame_delay_ms = (uint32_t)frame_delay_ms;
            } else
#endif
            if (paused_local && use_prefetched && s_front_buffer.ready) {
                frame_delay_ms = render_next_frame(&s_front_buffer, back_buffer, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, true);
                use_prefetched = false;
                if (frame_delay_ms < 0) frame_delay_ms = 100;
                s_target_frame_delay_ms = 100;
            } else if (!paused_local && s_front_buffer.ready) {
                prev_frame_delay_ms = s_target_frame_delay_ms;
                frame_delay_ms = render_next_frame(&s_front_buffer, back_buffer, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, use_prefetched);
                use_prefetched = false;
                if (frame_delay_ms < 0) frame_delay_ms = 1;
                s_target_frame_delay_ms = (uint32_t)frame_delay_ms;
                s_latest_frame_duration_ms = frame_delay_ms;

#if defined(CONFIG_P3A_LCD_DISPLAY_FRAME_DURATIONS)
                const int text_scale = 3;
                const int margin = text_scale * 2;
                app_lcd_color_t color_text = swap_requested ? color_red : color_white;
                draw_text_top_right(back_buffer, s_frame_duration_text, margin, margin, text_scale, color_text);
#endif
            } else {
                // No frame to render, reuse last displayed buffer
                back_buffer_idx = s_last_display_buffer;
                if (back_buffer_idx >= buffer_count) back_buffer_idx = 0;
                back_buffer = s_lcd_buffers[back_buffer_idx];
                frame_delay_ms = 100;
                s_target_frame_delay_ms = 100;
            }
        }

        // =====================================================================
        // Cache sync
        // =====================================================================
#if APP_LCD_HAVE_CACHE_MSYNC && defined(CONFIG_P3A_LCD_ENABLE_CACHE_FLUSH)
        esp_cache_msync(back_buffer, s_frame_buffer_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
#endif

        // =====================================================================
        // Flip buffers
        // =====================================================================
        s_last_display_buffer = back_buffer_idx;
        s_render_buffer_index = (back_buffer_idx + 1) % buffer_count;

        // =====================================================================
        // Wait for frame timing
        // =====================================================================
        if (!APP_LCD_MAX_SPEED_PLAYBACK_ENABLED) {
            const int64_t now_us = esp_timer_get_time();
            const int64_t processing_time_us = now_us - s_frame_processing_start_us;
            const int64_t target_delay_us = (int64_t)prev_frame_delay_ms * 1000;

            if (processing_time_us < target_delay_us) {
                const int64_t residual_us = target_delay_us - processing_time_us;
                if (residual_us > 1000) {
                    vTaskDelay(pdMS_TO_TICKS((residual_us + 500) / 1000));
                }
            }
        }

        // =====================================================================
        // DMA present
        // =====================================================================
        if (app_lcd_get_brightness() == 0) {
            memset(back_buffer, 0, s_frame_buffer_bytes);
#if APP_LCD_HAVE_CACHE_MSYNC && defined(CONFIG_P3A_LCD_ENABLE_CACHE_FLUSH)
            esp_cache_msync(back_buffer, s_frame_buffer_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
#endif
        }

        esp_lcd_panel_draw_bitmap(s_display_handle, 0, 0, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, back_buffer);

        // Update timing stats
        const int64_t present_time_us = esp_timer_get_time();
        if (s_last_frame_present_us != 0) {
            s_latest_frame_duration_ms = (int)((present_time_us - s_last_frame_present_us + 500) / 1000);
        }
        s_last_frame_present_us = present_time_us;

        if (s_last_duration_update_us == 0) s_last_duration_update_us = present_time_us;
        if ((present_time_us - s_last_duration_update_us) >= 500000) {
            snprintf(s_frame_duration_text, sizeof(s_frame_duration_text), "%d", s_latest_frame_duration_ms);
            s_last_duration_update_us = present_time_us;
        }

        // Yield if not using vsync
        if (!use_vsync) {
            vTaskDelay(1);
        }
    }
}


