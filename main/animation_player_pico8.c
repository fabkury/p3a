#include "animation_player_priv.h"

static uint8_t *s_pico8_frame_buffers[2] = { NULL, NULL };
static uint8_t s_pico8_decode_index = 0;
static uint8_t s_pico8_display_index = 0;
static bool s_pico8_frame_ready = false;
static bool s_pico8_override_active = false;
static int64_t s_pico8_last_frame_time_us = 0;
static uint16_t *s_pico8_lookup_x = NULL;
static uint16_t *s_pico8_lookup_y = NULL;
static bool s_pico8_palette_initialized = false;

static const pico8_color_t s_pico8_palette_defaults[PICO8_PALETTE_COLORS] = {
    {0x00, 0x00, 0x00}, {0x1D, 0x2B, 0x53}, {0x7E, 0x25, 0x53}, {0x00, 0x87, 0x51},
    {0xAB, 0x52, 0x36}, {0x5F, 0x57, 0x4F}, {0xC2, 0xC3, 0xC7}, {0xFF, 0xF1, 0xE8},
    {0xFF, 0x00, 0x4D}, {0xFF, 0xA3, 0x00}, {0xFF, 0xEC, 0x27}, {0x00, 0xE4, 0x36},
    {0x29, 0xAD, 0xFF}, {0x83, 0x76, 0x9C}, {0xFF, 0x77, 0xA8}, {0xFF, 0xCC, 0xAA},
};

static pico8_color_t s_pico8_palette[PICO8_PALETTE_COLORS];

esp_err_t ensure_pico8_resources(void)
{
    const size_t frame_bytes = (size_t)PICO8_FRAME_WIDTH * PICO8_FRAME_HEIGHT * 4;

    for (int i = 0; i < 2; ++i) {
        if (!s_pico8_frame_buffers[i]) {
            s_pico8_frame_buffers[i] = (uint8_t *)heap_caps_malloc(frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s_pico8_frame_buffers[i]) {
                ESP_LOGE(TAG, "Failed to allocate PICO-8 frame buffer %d", i);
                return ESP_ERR_NO_MEM;
            }
            memset(s_pico8_frame_buffers[i], 0, frame_bytes);
        }
    }

    if (!s_pico8_lookup_x) {
        s_pico8_lookup_x = (uint16_t *)heap_caps_malloc(EXAMPLE_LCD_H_RES * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
        if (!s_pico8_lookup_x) {
            ESP_LOGE(TAG, "Failed to allocate PICO-8 lookup X table");
            return ESP_ERR_NO_MEM;
        }
        for (int dst_x = 0; dst_x < EXAMPLE_LCD_H_RES; ++dst_x) {
            int src_x = (dst_x * PICO8_FRAME_WIDTH) / EXAMPLE_LCD_H_RES;
            if (src_x >= PICO8_FRAME_WIDTH) {
                src_x = PICO8_FRAME_WIDTH - 1;
            }
            s_pico8_lookup_x[dst_x] = (uint16_t)src_x;
        }
    }

    if (!s_pico8_lookup_y) {
        s_pico8_lookup_y = (uint16_t *)heap_caps_malloc(EXAMPLE_LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
        if (!s_pico8_lookup_y) {
            ESP_LOGE(TAG, "Failed to allocate PICO-8 lookup Y table");
            return ESP_ERR_NO_MEM;
        }
        for (int dst_y = 0; dst_y < EXAMPLE_LCD_V_RES; ++dst_y) {
            int src_y = (dst_y * PICO8_FRAME_HEIGHT) / EXAMPLE_LCD_V_RES;
            if (src_y >= PICO8_FRAME_HEIGHT) {
                src_y = PICO8_FRAME_HEIGHT - 1;
            }
            s_pico8_lookup_y[dst_y] = (uint16_t)src_y;
        }
    }

    if (!s_pico8_palette_initialized) {
        memcpy(s_pico8_palette, s_pico8_palette_defaults, sizeof(s_pico8_palette));
        s_pico8_palette_initialized = true;
    }

    return ESP_OK;
}

void release_pico8_resources(void)
{
    for (int i = 0; i < 2; ++i) {
        if (s_pico8_frame_buffers[i]) {
            free(s_pico8_frame_buffers[i]);
            s_pico8_frame_buffers[i] = NULL;
        }
    }
    if (s_pico8_lookup_x) {
        heap_caps_free(s_pico8_lookup_x);
        s_pico8_lookup_x = NULL;
    }
    if (s_pico8_lookup_y) {
        heap_caps_free(s_pico8_lookup_y);
        s_pico8_lookup_y = NULL;
    }
    s_pico8_frame_ready = false;
    s_pico8_override_active = false;
    s_pico8_last_frame_time_us = 0;
    s_pico8_palette_initialized = false;
}

bool pico8_stream_should_render(void)
{
    if (!s_pico8_override_active || !s_pico8_frame_ready) {
        return false;
    }

    bool active = false;
    int64_t now = esp_timer_get_time();

    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_pico8_override_active && s_pico8_frame_ready &&
            (now - s_pico8_last_frame_time_us) <= PICO8_STREAM_TIMEOUT_US) {
            active = true;
        } else if (s_pico8_override_active &&
                   (now - s_pico8_last_frame_time_us) > PICO8_STREAM_TIMEOUT_US) {
            s_pico8_override_active = false;
            s_pico8_frame_ready = false;
        }
        xSemaphoreGive(s_buffer_mutex);
    } else {
        if ((now - s_pico8_last_frame_time_us) <= PICO8_STREAM_TIMEOUT_US) {
            active = true;
        }
    }

    return active;
}

int render_pico8_frame(uint8_t *dest_buffer)
{
    if (!dest_buffer) {
        return -1;
    }

    if (ensure_pico8_resources() != ESP_OK) {
        return -1;
    }

    uint8_t *src = NULL;
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        src = s_pico8_frame_buffers[s_pico8_display_index];
        xSemaphoreGive(s_buffer_mutex);
    } else {
        src = s_pico8_frame_buffers[s_pico8_display_index];
    }

    if (!src) {
        return -1;
    }

    const int dst_h = EXAMPLE_LCD_V_RES;
    const int mid_row = dst_h / 2;

    s_upscale_src_buffer = src;
    s_upscale_dst_buffer = dest_buffer;
    s_upscale_lookup_x = s_pico8_lookup_x;
    s_upscale_lookup_y = s_pico8_lookup_y;
    s_upscale_src_w = PICO8_FRAME_WIDTH;
    s_upscale_src_h = PICO8_FRAME_HEIGHT;
    s_upscale_main_task = xTaskGetCurrentTaskHandle();

    s_upscale_worker_top_done = false;
    s_upscale_worker_bottom_done = false;

    s_upscale_row_start_top = 0;
    s_upscale_row_end_top = mid_row;
    s_upscale_row_start_bottom = mid_row;
    s_upscale_row_end_bottom = dst_h;

    MEMORY_BARRIER();

    if (s_upscale_worker_top) {
        xTaskNotify(s_upscale_worker_top, 1, eSetBits);
    }
    if (s_upscale_worker_bottom) {
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

    MEMORY_BARRIER();
    return 16;
}

esp_err_t animation_player_submit_pico8_frame(const uint8_t *palette_rgb, size_t palette_len,
                                              const uint8_t *pixel_data, size_t pixel_len)
{
    if (!pixel_data || pixel_len < PICO8_FRAME_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ensure_pico8_resources();
    if (err != ESP_OK) {
        return err;
    }

    if (palette_rgb && palette_len >= (PICO8_PALETTE_COLORS * 3)) {
        for (int i = 0; i < PICO8_PALETTE_COLORS; ++i) {
            size_t idx = (size_t)i * 3;
            s_pico8_palette[i].r = palette_rgb[idx + 0];
            s_pico8_palette[i].g = palette_rgb[idx + 1];
            s_pico8_palette[i].b = palette_rgb[idx + 2];
        }
    }

    const uint8_t target_index = s_pico8_decode_index & 0x01;
    uint8_t *target = s_pico8_frame_buffers[target_index];
    if (!target) {
        return ESP_ERR_INVALID_STATE;
    }

    const size_t total_pixels = (size_t)PICO8_FRAME_WIDTH * PICO8_FRAME_HEIGHT;
    size_t pixel_cursor = 0;
    for (size_t i = 0; i < PICO8_FRAME_BYTES && i < pixel_len; ++i) {
        uint8_t packed = pixel_data[i];
        uint8_t low_idx = packed & 0x0F;
        uint8_t high_idx = (packed >> 4) & 0x0F;

        pico8_color_t low_color = s_pico8_palette[low_idx];
        pico8_color_t high_color = s_pico8_palette[high_idx];

        if (pixel_cursor < total_pixels) {
            uint8_t *dst = target + pixel_cursor * 4;
            dst[0] = low_color.r;
            dst[1] = low_color.g;
            dst[2] = low_color.b;
            dst[3] = 0xFF;
            pixel_cursor++;
        }

        if (pixel_cursor < total_pixels) {
            uint8_t *dst = target + pixel_cursor * 4;
            dst[0] = high_color.r;
            dst[1] = high_color.g;
            dst[2] = high_color.b;
            dst[3] = 0xFF;
            pixel_cursor++;
        }
    }

    const int64_t now = esp_timer_get_time();

    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        s_pico8_display_index = target_index;
        s_pico8_decode_index = target_index ^ 1;
        s_pico8_frame_ready = true;
        s_pico8_override_active = true;
        s_pico8_last_frame_time_us = now;
        xSemaphoreGive(s_buffer_mutex);
    } else {
        s_pico8_display_index = target_index;
        s_pico8_decode_index = target_index ^ 1;
        s_pico8_frame_ready = true;
        s_pico8_override_active = true;
        s_pico8_last_frame_time_us = now;
    }

    return ESP_OK;
}

