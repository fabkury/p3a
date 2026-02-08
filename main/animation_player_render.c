// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-2025 p3a Contributors

#include "animation_player_priv.h"
#include "pico8_stream.h"
#include "pico8_render.h"
#include "ugfx_ui.h"
#include "view_tracker.h"
#include "play_scheduler.h"
#include "swap_future.h"
#include "config_store.h"
#include "debug_http_log.h"
#include <sys/time.h>

// Processing notification (from display_renderer_priv.h via weak symbol)
extern void proc_notif_success(void) __attribute__((weak));

// Frame rendering state
static bool s_use_prefetched = false;
static uint32_t s_target_frame_delay_ms = 100;

#if CONFIG_P3A_PERF_DEBUG
// Debug: Track target animation for performance analysis
static bool s_is_target_animation = false;
#endif

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
    // All decoders output RGB888; alpha is pre-composited against background at decode time
    return animation_decoder_decode_next_rgb(buf->decoder, dst);
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
#if CONFIG_P3A_PERF_DEBUG
    int64_t t_start = debug_timer_now_us();
#endif
    
    if (!buf || !buf->ready || !dest_buffer || !buf->decoder) {
        return -1;
    }

    // Use prefetched first frame: upscale directly from native_frame_b1 to dest_buffer
    // No intermediate buffer or memcpy needed - the decode was done during prefetch,
    // now we just upscale directly to the display back buffer
    if (use_prefetched && buf->first_frame_ready && buf->native_frame_b1) {
        display_renderer_parallel_upscale_rgb(buf->native_frame_b1, buf->upscale_src_w, buf->upscale_src_h,
                                              dest_buffer,
                                              buf->upscale_lookup_x, buf->upscale_lookup_y,
                                              buf->upscale_offset_x, buf->upscale_offset_y,
                                              buf->upscale_scaled_w, buf->upscale_scaled_h,
                                              buf->upscale_has_borders,
                                              display_renderer_get_rotation());
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

        display_renderer_parallel_upscale_rgb(buf->native_frame_b1, buf->upscale_src_w, buf->upscale_src_h,
                                              dest_buffer,
                                              buf->upscale_lookup_x, buf->upscale_lookup_y,
                                              buf->upscale_offset_x, buf->upscale_offset_y,
                                              buf->upscale_scaled_w, buf->upscale_scaled_h,
                                              buf->upscale_has_borders,
                                              display_renderer_get_rotation());
        return (int)buf->prefetched_first_frame_delay_ms;
    }

    uint8_t *decode_buffer = (buf->native_buffer_active == 0) ? buf->native_frame_b1 : buf->native_frame_b2;

#if CONFIG_P3A_PERF_DEBUG
    int64_t t_decode_start = debug_timer_now_us();
#endif
    
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

#if CONFIG_P3A_PERF_DEBUG
    int64_t t_decode_end = debug_timer_now_us();
    int64_t decode_time_us = t_decode_end - t_decode_start;
#endif
    
    uint32_t frame_delay_ms = 1;
    esp_err_t delay_err = animation_decoder_get_frame_delay(buf->decoder, &frame_delay_ms);
    if (delay_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get frame delay, using default");
        frame_delay_ms = 1;
    }
    buf->current_frame_delay_ms = frame_delay_ms;

    buf->native_buffer_active = (buf->native_buffer_active == 0) ? 1 : 0;

#if CONFIG_P3A_PERF_DEBUG
    int64_t t_upscale_start = debug_timer_now_us();
#endif

    // Use display_renderer for parallel upscaling with rotation
    display_renderer_parallel_upscale_rgb(decode_buffer, buf->upscale_src_w, buf->upscale_src_h,
                                          dest_buffer,
                                          buf->upscale_lookup_x, buf->upscale_lookup_y,
                                          buf->upscale_offset_x, buf->upscale_offset_y,
                                          buf->upscale_scaled_w, buf->upscale_scaled_h,
                                          buf->upscale_has_borders,
                                          display_renderer_get_rotation());

#if CONFIG_P3A_PERF_DEBUG
    int64_t t_upscale_end = debug_timer_now_us();
    int64_t upscale_time_us = t_upscale_end - t_upscale_start;
    int64_t total_time_us = t_upscale_end - t_start;
    
    // Record frame timing for aggregated stats
    debug_perf_record_frame(s_is_target_animation,
                           decode_time_us,
                           upscale_time_us,
                           total_time_us,
                           (int64_t)frame_delay_ms);
#endif

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
    // SAFETY: Set prefetch_in_progress BEFORE starting, so loader task can check it.
    // This is a critical section to prevent use-after-free heap corruption.
    if (back_buffer_prefetch_pending) {
        // Re-acquire mutex to safely check buffer state and set in_progress flag.
        bool buffer_valid = false;
        if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
            // Double-check that prefetch is still pending AND buffer is valid
            if (s_back_buffer.prefetch_pending && 
                s_back_buffer.decoder && 
                s_back_buffer.native_frame_b1) {
                buffer_valid = true;
                // CRITICAL: Mark that prefetch is now executing. The loader task
                // MUST check this flag before unloading the back buffer.
                s_back_buffer.prefetch_in_progress = true;
            } else if (!s_back_buffer.prefetch_pending) {
                ESP_LOGW(TAG, "Prefetch cancelled: prefetch_pending became false");
            }
            xSemaphoreGive(s_buffer_mutex);
        }
        
        if (!buffer_valid) {
            ESP_LOGE(TAG, "Prefetch aborted: back buffer invalid (decoder=%p, frame=%p, pending=%d)",
                     (void*)s_back_buffer.decoder, 
                     (void*)s_back_buffer.native_frame_b1,
                     (int)s_back_buffer.prefetch_pending);
            if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
                s_back_buffer.prefetch_pending = false;
                s_back_buffer.prefetch_in_progress = false;
                s_back_buffer.ready = false;
                s_swap_requested = false;
                xSemaphoreGive(s_buffer_mutex);
            }
            // Signal loader task that prefetch is done (aborted)
            if (s_prefetch_done_sem) {
                xSemaphoreGive(s_prefetch_done_sem);
            }
            goto skip_prefetch;
        }
        
        esp_err_t prefetch_err = prefetch_first_frame(&s_back_buffer);
        char failed_path[256] = {0};

        if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
            s_back_buffer.prefetch_pending = false;
            s_back_buffer.prefetch_in_progress = false;  // Prefetch done, safe to unload
            s_back_buffer.ready = (prefetch_err == ESP_OK);
            swap_requested = s_swap_requested;
            back_buffer_ready = s_back_buffer.ready;

            if (s_back_buffer.filepath) {
                strlcpy(failed_path, s_back_buffer.filepath, sizeof(failed_path));
            }

            // If prefetch failed, clear swap request so we don't get stuck.
            if (prefetch_err != ESP_OK) {
                s_swap_requested = false;
            }
            xSemaphoreGive(s_buffer_mutex);
        }

        // Signal loader task that prefetch is done
        if (s_prefetch_done_sem) {
            xSemaphoreGive(s_prefetch_done_sem);
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

            // No auto-retry or navigation on prefetch failure
            ESP_LOGW(TAG, "Prefetch failed: %s", esp_err_to_name(prefetch_err));
        }
    }

skip_prefetch:
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
            s_back_buffer.prefetch_in_progress = false;
            
#if CONFIG_P3A_PERF_DEBUG
            // Flush stats before animation change, then check if new animation is target
            debug_perf_flush_stats();
            s_is_target_animation = false;
            if (s_front_buffer.filepath && strstr(s_front_buffer.filepath, "e7fbb22e-3c16-46bd-b488-53ab8dc4c524") != NULL) {
                s_is_target_animation = true;
                ESP_LOGI(TAG, "PERF: Target animation loaded (sonic_animation)");
            }
            // Log animation dimensions for analysis
            ESP_LOGI(TAG, "PERF_DIM: native=%dx%d upscale_src=%dx%d scaled=%dx%d offset=%d,%d transp=%d",
                     s_front_buffer.decoder_info.canvas_width,
                     s_front_buffer.decoder_info.canvas_height,
                     s_front_buffer.upscale_src_w, s_front_buffer.upscale_src_h,
                     s_front_buffer.upscale_scaled_w, s_front_buffer.upscale_scaled_h,
                     s_front_buffer.upscale_offset_x, s_front_buffer.upscale_offset_y,
                     s_front_buffer.decoder_info.has_transparency);
#endif
            
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
            
            ESP_LOGI(TAG, "Buffers swapped: now playing %s",
                     s_front_buffer.filepath ? s_front_buffer.filepath : "(unknown)");

            // Clear processing notification - successful swap
            if (proc_notif_success) {
                proc_notif_success();
            }

            // DEBUG: Log decode path for each artwork (remove this block when done investigating)
#if 0
            ESP_LOGI(TAG, "DEBUG_DECODE_PATH: has_transparency=%d (%s), file=%s",
                     s_front_buffer.decoder_info.has_transparency,
                     s_front_buffer.decoder_info.has_transparency ? "alpha-blend-at-decode" : "rgb-copy",
                     s_front_buffer.filepath ? s_front_buffer.filepath : "(unknown)");
#endif

            // Clear any "Loading channel" or "Updating index" message now that playback has started
            extern void p3a_render_set_channel_message(const char *channel_name, int msg_type,
                                                       int progress_percent, const char *detail) __attribute__((weak));
            if (p3a_render_set_channel_message) {
                p3a_render_set_channel_message(NULL, 0 /* P3A_CHANNEL_MSG_NONE */, -1, NULL);
            }

            if (s_front_buffer.is_live_mode_swap) {
                live_mode_notify_swap_succeeded(s_front_buffer.live_index);
            }
            
            // Notify play_scheduler that swap succeeded (resets dwell timer)
            play_scheduler_reset_timer();
            
            // Mark successful swap for auto-retry safeguard
            animation_loader_mark_swap_successful();
            
            // Update playback controller with new animation metadata
            if (s_front_buffer.filepath) {
                playback_controller_set_animation_metadata(s_front_buffer.filepath, true);
            }
            
            // Signal view tracker with the artwork info captured at swap time
            view_tracker_signal_swap(s_front_buffer.post_id, s_front_buffer.filepath);
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
    if (paused_local) {
        // Paused: output black frame. Buffer management (prefetch + swap)
        // already ran above so animations load silently in the background.
        // Don't consume s_use_prefetched -- leave it for resume.
        memset(dest_buffer, 0, EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * (EXAMPLE_LCD_BIT_PER_PIXEL / 8));
        s_target_frame_delay_ms = 100;
        return 100;
    } else if (s_front_buffer.ready) {
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

// p3a_render integration: provide a stride-taking wrapper for the state-aware renderer.
// p3a_render expects this symbol (weak-linked).
int animation_player_render_frame_internal(uint8_t *buffer, size_t stride)
{
    (void)stride; // animation renderer already knows how to write a full display frame
    return animation_player_render_frame_callback(buffer, NULL);
}
