#include "animation_player_priv.h"
#include "pico8_stream.h"
#include "pico8_render.h"
#include "ugfx_ui.h"
#include "makapix.h"
#include "makapix_api.h"

// Frame rendering state
static bool s_use_prefetched = false;
static uint32_t s_target_frame_delay_ms = 100;

/**
 * @brief Decode the next animation frame and upscale to display buffer
 */
static int render_next_frame(animation_buffer_t *buf, uint8_t *dest_buffer, int target_w, int target_h, bool use_prefetched)
{
    if (!buf || !buf->ready || !dest_buffer || !buf->decoder) {
        return -1;
    }

    // Use prefetched first frame: upscale directly from native_frame_b1 to dest_buffer
    // No intermediate buffer or memcpy needed - the decode was done during prefetch,
    // now we just upscale directly to the display back buffer
    if (use_prefetched && buf->first_frame_ready && buf->native_frame_b1) {
        display_renderer_parallel_upscale(buf->native_frame_b1, buf->upscale_src_w, buf->upscale_src_h,
                                          dest_buffer,
                                          buf->upscale_lookup_x, buf->upscale_lookup_y,
                                          display_renderer_get_rotation());
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

    // Use display_renderer for parallel upscaling with rotation
    display_renderer_parallel_upscale(decode_buffer, buf->upscale_src_w, buf->upscale_src_h,
                                      dest_buffer,
                                      buf->upscale_lookup_x, buf->upscale_lookup_y,
                                      display_renderer_get_rotation());

    return (int)buf->current_frame_delay_ms;
}

/**
 * @brief Prefetch the first frame of an animation
 * 
 * This decodes the first frame to native_frame_b1 but does NOT upscale yet.
 * The upscale happens later when render_next_frame() is called with use_prefetched=true,
 * at which point we upscale directly to the display back buffer (no intermediate copy).
 */
esp_err_t prefetch_first_frame(animation_buffer_t *buf)
{
    if (!buf || !buf->decoder || !buf->native_frame_b1) {
        return ESP_ERR_INVALID_ARG;
    }

    // Decode first frame to native_frame_b1
    // The upscale will happen later, directly to the display back buffer
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

    // NO upscale here - that happens in render_next_frame() directly to dest_buffer

    buf->first_frame_ready = true;
    buf->decoder_at_frame_1 = true;

    ESP_LOGD(TAG, "Prefetched first frame for animation index %zu", buf->asset_index);

    return ESP_OK;
}

/**
 * @brief Frame callback for display_renderer
 * 
 * This function is called by the display render task to get each frame.
 * It handles animation playback, buffer swapping, prefetching, and PICO-8 rendering.
 */
int animation_player_render_frame_callback(uint8_t *dest_buffer, void *user_ctx)
{
    (void)user_ctx;

    if (!dest_buffer) {
        return -1;
    }

    int frame_delay_ms = 100;

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
            
            // Update playback controller with new animation metadata
            if (s_front_buffer.filepath) {
                playback_controller_set_animation_metadata(s_front_buffer.filepath, true);
            }
            
            // Submit view tracking for Makapix artworks
            int32_t post_id = makapix_get_current_post_id();
            if (post_id > 0) {
                // Check if this is from vault (Makapix artwork)
                if (s_front_buffer.filepath && strstr(s_front_buffer.filepath, "/vault/") != NULL) {
                    // Get intent: intentional for show_artwork commands, automated otherwise
                    bool is_intentional = makapix_get_and_clear_view_intent();
                    makapix_view_intent_t intent = is_intentional ? MAKAPIX_VIEW_INTENT_INTENTIONAL : MAKAPIX_VIEW_INTENT_AUTOMATED;
                    
                    makapix_api_submit_view(post_id, intent);
                    ESP_LOGD(TAG, "Submitted view for post_id=%ld (intent=%s)", post_id, 
                             is_intentional ? "intentional" : "automated");
                }
            }
        }
        s_use_prefetched = true;
    }

    // Check for PICO-8 mode
#if CONFIG_P3A_PICO8_ENABLE
    bool pico8_active = pico8_stream_is_active();
    if (pico8_active) {
        size_t row_stride;
        display_renderer_get_dimensions(NULL, NULL, &row_stride);
        frame_delay_ms = pico8_render_frame(dest_buffer, row_stride);
        if (frame_delay_ms < 0) frame_delay_ms = 16;
        s_target_frame_delay_ms = (uint32_t)frame_delay_ms;
        return frame_delay_ms;
    }
#endif

    // Render animation frame
    if (paused_local && s_use_prefetched && s_front_buffer.ready) {
        // When paused, show the prefetched frame once then hold
        frame_delay_ms = render_next_frame(&s_front_buffer, dest_buffer, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, true);
        s_use_prefetched = false;
        if (frame_delay_ms < 0) frame_delay_ms = 100;
        s_target_frame_delay_ms = 100;
    } else if (!paused_local && s_front_buffer.ready) {
        // Normal playback
        frame_delay_ms = render_next_frame(&s_front_buffer, dest_buffer, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, s_use_prefetched);
        s_use_prefetched = false;
        if (frame_delay_ms < 0) frame_delay_ms = 1;
        s_target_frame_delay_ms = (uint32_t)frame_delay_ms;
    } else {
        // No valid frame - return error so display_renderer shows black or last frame
        return -1;
    }

    return frame_delay_ms;
}
