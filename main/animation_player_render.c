#include "animation_player_priv.h"
#include "pico8_stream.h"
#include "pico8_render.h"
#include "ugfx_ui.h"
#include "makapix.h"
#include "makapix_api.h"
#include "channel_player.h"
#include "swap_future.h"
#include "config_store.h"
#include <sys/time.h>

// Frame rendering state
static bool s_use_prefetched = false;
static uint32_t s_target_frame_delay_ms = 100;

// Rotation-dependent lookup maps must be rebuilt when rotation changes.
static volatile bool s_upscale_maps_rebuild_pending = false;
static volatile display_rotation_t s_upscale_maps_rebuild_rotation = DISPLAY_ROTATION_0;

void animation_player_render_on_rotation_changed(display_rotation_t rotation)
{
    s_upscale_maps_rebuild_rotation = rotation;
    s_upscale_maps_rebuild_pending = true;
}

static inline esp_err_t decode_next_native(animation_buffer_t *buf, uint8_t *dst)
{
    if (!buf || !buf->decoder || !dst) {
        return ESP_ERR_INVALID_ARG;
    }
    if (buf->native_bytes_per_pixel == 3) {
        return animation_decoder_decode_next_rgb(buf->decoder, dst);
    }
    return animation_decoder_decode_next(buf->decoder, dst);
}

static uint64_t wall_clock_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static esp_err_t prefetch_first_frame_seeked(animation_buffer_t *buf, uint32_t start_frame, uint64_t start_time_ms)
{
    if (!buf || !buf->decoder || !buf->native_frame_b1 || !buf->native_frame_b2) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t frame_count = (uint32_t)buf->decoder_info.frame_count;
    if (frame_count <= 1) {
        // Still images: no seeking required; fall back to normal prefetch path.
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Derive elapsed time from ideal wall-clock start, if provided.
    uint32_t elapsed_ms = 0;
    if (start_time_ms != 0) {
        const uint64_t now_ms = wall_clock_ms();
        elapsed_ms = (now_ms > start_time_ms) ? (uint32_t)MIN(now_ms - start_time_ms, (uint64_t)UINT32_MAX) : 0;
    }

    // If explicit start_frame is provided (and no start_time_ms), use it.
    bool use_frame_seek = (start_time_ms == 0 && start_frame > 0);

    // Always reset before seeking.
    esp_err_t err = animation_decoder_reset(buf->decoder);
    if (err != ESP_OK) {
        return err;
    }

    // Compute intrinsic loop duration (ms) so we can modulo large elapsed offsets.
    uint32_t loop_ms = 0;
    if (!use_frame_seek && elapsed_ms > 0) {
        uint64_t total = 0;
        for (uint32_t i = 0; i < frame_count; i++) {
            err = decode_next_native(buf, buf->native_frame_b2);
            if (err != ESP_OK) {
                break;
            }
            uint32_t d = 1;
            (void)animation_decoder_get_frame_delay(buf->decoder, &d);
            if (d < 1) d = 1;
            total += d;
        }
        loop_ms = (total > 0) ? (uint32_t)MIN(total, (uint64_t)UINT32_MAX) : 0;

        // Reset again for the actual seek.
        (void)animation_decoder_reset(buf->decoder);
        if (err != ESP_OK) {
            // Fall back to a non-seeked prefetch.
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (loop_ms > 0) {
            elapsed_ms = elapsed_ms % loop_ms;
        } else {
            elapsed_ms = 0;
        }
    }

    // Seek by decoding and discarding frames until we reach the desired position.
    uint32_t spent = 0;
    uint32_t desired_frame_delay_ms = 1;

    if (use_frame_seek) {
        const uint32_t target = start_frame % frame_count;
        for (uint32_t i = 0; i < target; i++) {
            err = decode_next_native(buf, buf->native_frame_b2);
            if (err != ESP_OK) return err;
        }
    } else if (elapsed_ms > 0) {
        while (true) {
            err = decode_next_native(buf, buf->native_frame_b2);
            if (err != ESP_OK) return err;
            uint32_t d = 1;
            (void)animation_decoder_get_frame_delay(buf->decoder, &d);
            if (d < 1) d = 1;
            if ((uint64_t)spent + (uint64_t)d > (uint64_t)elapsed_ms) {
                // This decoded frame is the correct one for the elapsed offset.
                desired_frame_delay_ms = d;
                memcpy(buf->native_frame_b1, buf->native_frame_b2, buf->native_frame_size);
                break;
            }
            spent += d;
        }
        // We already have the desired first frame in native_frame_b1.
        buf->prefetched_first_frame_delay_ms = desired_frame_delay_ms;
        buf->first_frame_ready = true;
        buf->decoder_at_frame_1 = true;
        buf->start_time_ms = 0;
        buf->start_frame = 0;
        return ESP_OK;
    }

    // Frame-based seek: decode the desired first frame now.
    err = decode_next_native(buf, buf->native_frame_b1);
    if (err != ESP_OK) return err;
    (void)animation_decoder_get_frame_delay(buf->decoder, &desired_frame_delay_ms);
    if (desired_frame_delay_ms < 1) desired_frame_delay_ms = 1;
    buf->prefetched_first_frame_delay_ms = desired_frame_delay_ms;
    buf->first_frame_ready = true;
    buf->decoder_at_frame_1 = true;
    buf->start_time_ms = 0;
    buf->start_frame = 0;
    return ESP_OK;
}

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
        if (buf->native_bytes_per_pixel == 3) {
            display_renderer_parallel_upscale_rgb(buf->native_frame_b1, buf->upscale_src_w, buf->upscale_src_h,
                                                  dest_buffer,
                                                  buf->upscale_lookup_x, buf->upscale_lookup_y,
                                                  buf->upscale_offset_x, buf->upscale_offset_y,
                                                  buf->upscale_scaled_w, buf->upscale_scaled_h,
                                                  buf->upscale_has_borders,
                                                  display_renderer_get_rotation());
        } else {
            display_renderer_parallel_upscale(buf->native_frame_b1, buf->upscale_src_w, buf->upscale_src_h,
                                              dest_buffer,
                                              buf->upscale_lookup_x, buf->upscale_lookup_y,
                                              buf->upscale_offset_x, buf->upscale_offset_y,
                                              buf->upscale_scaled_w, buf->upscale_scaled_h,
                                              buf->upscale_has_borders,
                                              display_renderer_get_rotation());
        }
        buf->first_frame_ready = false;
        // Static images: keep using native_frame_b1 without re-decoding each tick
        if (buf->decoder_info.frame_count <= 1) {
            buf->static_frame_cached = true;
            buf->static_bg_generation = config_store_get_background_color_generation();
        }
        return (int)buf->prefetched_first_frame_delay_ms;
    }

    if (!buf->native_frame_b1 || !buf->native_frame_b2) {
        ESP_LOGE(TAG, "Native frame buffers not allocated");
        return -1;
    }

    // Static image fast path: reuse cached native_frame_b1 every frame (no re-decode),
    // but if background changes AND the asset has transparency, refresh compositing once.
    if (buf->decoder_info.frame_count <= 1 && buf->native_frame_b1) {
        if (!buf->static_frame_cached) {
            esp_err_t derr = decode_next_native(buf, buf->native_frame_b1);
            if (derr != ESP_OK) {
                ESP_LOGE(TAG, "Failed to decode static frame: %s", esp_err_to_name(derr));
                return -1;
            }
            uint32_t d = 1;
            (void)animation_decoder_get_frame_delay(buf->decoder, &d);
            if (d < 1) d = 1;
            buf->prefetched_first_frame_delay_ms = d;
            buf->static_frame_cached = true;
            buf->static_bg_generation = config_store_get_background_color_generation();
        } else if (buf->decoder_info.has_transparency) {
            const uint32_t gen = config_store_get_background_color_generation();
            if (gen != buf->static_bg_generation) {
                // Background changed: re-decode once so decoder can re-composite against the new background.
                esp_err_t derr = decode_next_native(buf, buf->native_frame_b1);
                if (derr != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to refresh static frame after bg change: %s", esp_err_to_name(derr));
                    return -1;
                }
                buf->static_bg_generation = gen;
            }
        }

        if (buf->native_bytes_per_pixel == 3) {
            display_renderer_parallel_upscale_rgb(buf->native_frame_b1, buf->upscale_src_w, buf->upscale_src_h,
                                                  dest_buffer,
                                                  buf->upscale_lookup_x, buf->upscale_lookup_y,
                                                  buf->upscale_offset_x, buf->upscale_offset_y,
                                                  buf->upscale_scaled_w, buf->upscale_scaled_h,
                                                  buf->upscale_has_borders,
                                                  display_renderer_get_rotation());
        } else {
            display_renderer_parallel_upscale(buf->native_frame_b1, buf->upscale_src_w, buf->upscale_src_h,
                                              dest_buffer,
                                              buf->upscale_lookup_x, buf->upscale_lookup_y,
                                              buf->upscale_offset_x, buf->upscale_offset_y,
                                              buf->upscale_scaled_w, buf->upscale_scaled_h,
                                              buf->upscale_has_borders,
                                              display_renderer_get_rotation());
        }
        return (int)buf->prefetched_first_frame_delay_ms;
    }

    uint8_t *decode_buffer = (buf->native_buffer_active == 0) ? buf->native_frame_b1 : buf->native_frame_b2;

    esp_err_t err = decode_next_native(buf, decode_buffer);
    if (err == ESP_ERR_INVALID_STATE) {
        animation_decoder_reset(buf->decoder);
        err = decode_next_native(buf, decode_buffer);
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
    if (buf->native_bytes_per_pixel == 3) {
        display_renderer_parallel_upscale_rgb(decode_buffer, buf->upscale_src_w, buf->upscale_src_h,
                                              dest_buffer,
                                              buf->upscale_lookup_x, buf->upscale_lookup_y,
                                              buf->upscale_offset_x, buf->upscale_offset_y,
                                              buf->upscale_scaled_w, buf->upscale_scaled_h,
                                              buf->upscale_has_borders,
                                              display_renderer_get_rotation());
    } else {
        display_renderer_parallel_upscale(decode_buffer, buf->upscale_src_w, buf->upscale_src_h,
                                          dest_buffer,
                                          buf->upscale_lookup_x, buf->upscale_lookup_y,
                                          buf->upscale_offset_x, buf->upscale_offset_y,
                                          buf->upscale_scaled_w, buf->upscale_scaled_h,
                                          buf->upscale_has_borders,
                                          display_renderer_get_rotation());
    }

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

    // If Live Mode / swap_future provided a start alignment, prefetch the correctly-aligned first frame.
    if ((buf->start_time_ms != 0) || (buf->start_frame != 0)) {
        esp_err_t seek_err = prefetch_first_frame_seeked(buf, buf->start_frame, buf->start_time_ms);
        if (seek_err == ESP_OK) {
            ESP_LOGD(TAG, "Prefetched seeked first frame (start_time_ms=%llu start_frame=%u)",
                     (unsigned long long)buf->start_time_ms, (unsigned)buf->start_frame);
            return ESP_OK;
        }
        // If seek is not supported or fails, fall back to frame 0 prefetch.
        ESP_LOGW(TAG, "Seeked prefetch failed (%s). Falling back to frame 0.", esp_err_to_name(seek_err));
        buf->start_time_ms = 0;
        buf->start_frame = 0;
        (void)animation_decoder_reset(buf->decoder);
    }

    // Decode first frame to native_frame_b1
    // The upscale will happen later, directly to the display back buffer
    uint8_t *decode_buffer = buf->native_frame_b1;
    esp_err_t err = decode_next_native(buf, decode_buffer);
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

    // If rotation changed since the last frame, rebuild upscale maps at a safe point (render task context).
    if (s_upscale_maps_rebuild_pending) {
        const display_rotation_t rot = s_upscale_maps_rebuild_rotation;
        if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
            if (s_front_buffer.decoder) {
                (void)animation_loader_rebuild_upscale_maps(&s_front_buffer, rot);
            }
            // IMPORTANT: Do not touch the back buffer while the loader task is busy mutating it.
            // Rebuilding maps involves heap free/alloc and can race with loader load/unload,
            // corrupting the heap (later crashes inside tlsf_free).
            if (!s_loader_busy && s_back_buffer.decoder) {
                (void)animation_loader_rebuild_upscale_maps(&s_back_buffer, rot);
            }
            xSemaphoreGive(s_buffer_mutex);
        }
        s_upscale_maps_rebuild_pending = false;
    }

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
        bool was_live = false;
        uint32_t failed_live_idx = 0;
        char failed_path[256] = {0};

        if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
            s_back_buffer.prefetch_pending = false;
            s_back_buffer.ready = (prefetch_err == ESP_OK);
            swap_requested = s_swap_requested;
            back_buffer_ready = s_back_buffer.ready;

            was_live = s_back_buffer.is_live_mode_swap;
            failed_live_idx = s_back_buffer.live_index;
            if (s_back_buffer.filepath) {
                strlcpy(failed_path, s_back_buffer.filepath, sizeof(failed_path));
            }

            // If prefetch failed, clear swap request so we don't get stuck.
            if (prefetch_err != ESP_OK) {
                s_swap_requested = false;
            }
            xSemaphoreGive(s_buffer_mutex);
        }

        if (prefetch_err != ESP_OK) {
            ESP_LOGW(TAG, "Prefetch failed: %s", esp_err_to_name(prefetch_err));

            // Attempt to delete corrupt vault files (safeguarded).
            if (failed_path[0] != '\0') {
                (void)animation_loader_try_delete_corrupt_vault_file(failed_path, prefetch_err);
            }

            // Clean up back buffer contents so future attempts are clean.
            if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
                unload_animation_buffer(&s_back_buffer);
                xSemaphoreGive(s_buffer_mutex);
            }

            if (was_live) {
                void *nav = channel_player_get_navigator();
                if (nav) {
                    (void)live_mode_recover_from_failed_swap(nav, failed_live_idx, prefetch_err);
                }
            } else {
                // Non-live swap: advance and retry to avoid stalling.
                (void)channel_player_advance();
                (void)animation_player_request_swap_current();
            }
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
            
            // Check if newly-swapped front buffer was built for a different rotation than current.
            // If so, rebuild its upscale maps immediately (before rendering with stale tables).
            const display_rotation_t current_rotation = display_renderer_get_rotation();
            if (s_front_buffer.decoder &&
                s_front_buffer.upscale_rotation_built != current_rotation) {
                ESP_LOGI(TAG, "Rebuilding upscale maps for newly-swapped buffer (built for %d, current %d)",
                         (int)s_front_buffer.upscale_rotation_built, (int)current_rotation);
                (void)animation_loader_rebuild_upscale_maps(&s_front_buffer, current_rotation);
            }
            xSemaphoreGive(s_buffer_mutex);
            
            ESP_LOGI(TAG, "Buffers swapped: front now playing index %zu", s_front_buffer.asset_index);

            if (s_front_buffer.is_live_mode_swap) {
                live_mode_notify_swap_succeeded(s_front_buffer.live_index);
            }
            
            // Mark successful swap for auto-retry safeguard
            animation_loader_mark_swap_successful();
            
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
