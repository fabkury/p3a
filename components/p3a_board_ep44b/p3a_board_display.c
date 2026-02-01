/**
 * @file p3a_board_display.c
 * @brief Display hardware implementation for EP44B board
 */

#include "p3a_board.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_log.h"
#include "esp_lcd_mipi_dsi.h"

static const char *TAG = "p3a_board_display";

// Display state
static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;
static uint8_t *s_buffers[P3A_DISPLAY_BUFFERS] = { NULL };
static uint8_t s_buffer_count = 0;
static size_t s_row_stride = 0;
static size_t s_buffer_bytes = 0;
static int s_brightness = 100;
static bool s_initialized = false;

esp_err_t p3a_board_display_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Display already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing %s display (%dx%d, %d-bit)",
             P3A_BOARD_NAME, P3A_DISPLAY_WIDTH, P3A_DISPLAY_HEIGHT, P3A_DISPLAY_BPP);

    // Initialize display panel using BSP
    bsp_display_config_t config = { 0 };
    esp_err_t err = bsp_display_new(&config, &s_panel, &s_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create display: %s", esp_err_to_name(err));
        return err;
    }

    // Initialize brightness control
#if P3A_HAS_BRIGHTNESS
    err = bsp_display_brightness_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Brightness init failed: %s (continuing without)", esp_err_to_name(err));
    } else {
        s_brightness = 100;
        bsp_display_brightness_set(s_brightness);
    }
#endif

    // Get framebuffers from DPI panel (variadic API requires compile-time switch)
    switch (P3A_DISPLAY_BUFFERS) {
        case 1:
            err = esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1,
                (void **)&s_buffers[0]);
            break;
        case 2:
            err = esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2,
                (void **)&s_buffers[0], (void **)&s_buffers[1]);
            break;
        case 3:
            err = esp_lcd_dpi_panel_get_frame_buffer(s_panel, 3,
                (void **)&s_buffers[0], (void **)&s_buffers[1],
                (void **)&s_buffers[2]);
            break;
        default:
            ESP_LOGE(TAG, "Unsupported buffer count: %d", P3A_DISPLAY_BUFFERS);
            return ESP_ERR_NOT_SUPPORTED;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get framebuffers: %s", esp_err_to_name(err));
        return err;
    }

    s_buffer_count = P3A_DISPLAY_BUFFERS;

    // Calculate row stride and buffer size
    const size_t bytes_per_pixel = P3A_DISPLAY_BPP / 8;
    s_row_stride = (size_t)P3A_DISPLAY_WIDTH * bytes_per_pixel;
    s_buffer_bytes = s_row_stride * P3A_DISPLAY_HEIGHT;

    // Check if hardware uses padding (detect from buffer spacing)
    if (s_buffer_count > 1 && s_buffers[0] && s_buffers[1] && s_buffers[1] > s_buffers[0]) {
        const size_t spacing = (size_t)(s_buffers[1] - s_buffers[0]);
        if (spacing > 0 && (spacing % P3A_DISPLAY_HEIGHT) == 0) {
            const size_t detected_stride = spacing / P3A_DISPLAY_HEIGHT;
            if (detected_stride >= s_row_stride) {
                s_row_stride = detected_stride;
                s_buffer_bytes = spacing;
            }
        }
    }

    ESP_LOGI(TAG, "Display initialized: %d buffers, stride=%zu, size=%zu bytes",
             s_buffer_count, s_row_stride, s_buffer_bytes);

    s_initialized = true;
    return ESP_OK;
}

esp_lcd_panel_handle_t p3a_board_get_panel(void)
{
    return s_panel;
}

uint8_t* p3a_board_get_buffer(int index)
{
    if (index < 0 || index >= s_buffer_count) {
        return NULL;
    }
    return s_buffers[index];
}

uint8_t p3a_board_get_buffer_count(void)
{
    return s_buffer_count;
}

size_t p3a_board_get_row_stride(void)
{
    return s_row_stride;
}

size_t p3a_board_get_buffer_bytes(void)
{
    return s_buffer_bytes;
}

// ============================================================================
// Brightness control
// ============================================================================

esp_err_t p3a_board_set_brightness(int percent)
{
#if P3A_HAS_BRIGHTNESS
    // Clamp to valid range
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    // Apply cubic easing (ease-in): slow near 0%, faster near 100%
    // This provides a more natural brightness curve
    float normalized = (float)percent / 100.0f;
    float eased = normalized * normalized * normalized;
    int hw_brightness = (int)(eased * 100.0f + 0.5f);

    if (hw_brightness < 0) hw_brightness = 0;
    if (hw_brightness > 100) hw_brightness = 100;

    esp_err_t err = bsp_display_brightness_set(hw_brightness);
    if (err == ESP_OK) {
        s_brightness = percent;
    }
    return err;
#else
    (void)percent;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

int p3a_board_get_brightness(void)
{
    return s_brightness;
}

esp_err_t p3a_board_adjust_brightness(int delta_percent)
{
    return p3a_board_set_brightness(s_brightness + delta_percent);
}

// ============================================================================
// Touch initialization
// ============================================================================

#if P3A_HAS_TOUCH
#include "bsp/touch.h"

esp_err_t p3a_board_touch_init(esp_lcd_touch_handle_t *handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    return bsp_touch_new(NULL, handle);
}
#endif

