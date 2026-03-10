# Sub-Frame Rendering Implementation Plan

## Context

p3a's render loop ties screen refresh 1:1 to animation frame rate. Animations with long frame durations (e.g. 500ms+) block the screen from updating — overlays like processing notifications can't appear until the next animation frame is due. This plan implements sub-frame rendering: splitting long animation frames into multiple screen updates (minimum 8 FPS / max 125ms per screen frame), decoding the native frame once and re-upscaling it for each sub-frame.

Full spec: `docs/sub-frame-rendering/SPEC.md`

## Files to Modify

| File | Changes |
|------|---------|
| `main/display_renderer_priv.h` | Add `display_subframe_info_t` struct, constant, extern |
| `main/display_renderer.c` | Add global, re-upscale helper, sub-frame loop in render task |
| `main/animation_player_render.c` | Populate sub-frame info after `render_next_frame()` |

## Step 1: Add sub-frame info struct to `display_renderer_priv.h`

After the processing notification section (~line 186), add:

```c
// ============================================================================
// Sub-frame rendering
// ============================================================================

#define DISPLAY_MAX_FRAME_DURATION_MS 125

typedef struct {
    const uint8_t *native_buffer;  // Cached decoded native frame
    int src_w, src_h;              // Native dimensions
    const uint16_t *lookup_x;     // CPU upscale lookup tables
    const uint16_t *lookup_y;
    int offset_x, offset_y;       // Centering offsets (letterbox/pillarbox)
    int scaled_w, scaled_h;       // Scaled image dimensions
    bool has_borders;              // Whether to fill border regions
    bool use_ppa;                  // true = PPA bilinear, false = CPU nearest-neighbor
    int dst_w, dst_h;             // Display dimensions (for PPA path)
    bool valid;                    // true = data populated, eligible for sub-framing
} display_subframe_info_t;

extern display_subframe_info_t g_subframe_info;
```

Use `bool use_ppa` instead of `ps_channel_type_t` to avoid coupling display_renderer to play_scheduler_types.h.

## Step 2: Add global and re-upscale helper to `display_renderer.c`

### 2a: Global definition (after processing notification state, ~line 88)

```c
display_subframe_info_t g_subframe_info = { .valid = false };
```

### 2b: Conditional PPA include (at top of file)

```c
#if CONFIG_P3A_PPA_UPSCALE_ENABLE
#include "display_ppa_upscaler.h"
#endif
```

### 2c: Re-upscale helper (before `display_render_task`)

```c
static void subframe_reupscale(const display_subframe_info_t *info, uint8_t *dst)
{
#if CONFIG_P3A_PPA_UPSCALE_ENABLE
    if (info->use_ppa) {
        esp_err_t err = display_ppa_upscale_rgb(
            info->native_buffer, info->src_w, info->src_h,
            dst, info->dst_w, info->dst_h, g_screen_rotation);
        if (err == ESP_OK) return;
        // PPA failed, fall through to CPU
    }
#endif
    display_renderer_parallel_upscale_rgb(
        info->native_buffer, info->src_w, info->src_h, dst,
        info->lookup_x, info->lookup_y,
        info->offset_x, info->offset_y,
        info->scaled_w, info->scaled_h,
        info->has_borders, g_screen_rotation);
}
```

## Step 3: Populate sub-frame info in `animation_player_render.c`

### 3a: Extern declaration (near top, after includes)

```c
#include "display_renderer_priv.h"  // for display_subframe_info_t, g_subframe_info
```

Or if that creates include cycles, use a bare extern:
```c
extern struct display_subframe_info_t g_subframe_info; // defined in display_renderer.c
```

### 3b: Populate after successful render_next_frame()

In `animation_player_render_frame_callback()`, after the `s_front_buffer.ready` branch (around line 616), after `render_next_frame()` succeeds:

```c
} else if (s_front_buffer.ready) {
    frame_delay_ms = render_next_frame(&s_front_buffer, dest_buffer,
                                        EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES,
                                        s_use_prefetched);
    s_use_prefetched = false;
    if (frame_delay_ms < 0) frame_delay_ms = 1;
    s_target_frame_delay_ms = (uint32_t)frame_delay_ms;

    // --- NEW: Populate sub-frame info for re-upscaling ---
    // Determine which native buffer was just upscaled FROM:
    // - Static/prefetched: always native_frame_b1
    // - Animated: render_next_frame() toggles native_buffer_active AFTER
    //   selecting decode_buffer, so the buffer just used is the one
    //   OPPOSITE of the current active index.
    const uint8_t *rendered_native;
    if (s_front_buffer.static_frame_cached || s_front_buffer.first_frame_ready) {
        rendered_native = s_front_buffer.native_frame_b1;
    } else {
        rendered_native = (s_front_buffer.native_buffer_active != 0)
            ? s_front_buffer.native_frame_b1
            : s_front_buffer.native_frame_b2;
    }

    g_subframe_info.native_buffer = rendered_native;
    g_subframe_info.src_w = s_front_buffer.upscale_src_w;
    g_subframe_info.src_h = s_front_buffer.upscale_src_h;
    g_subframe_info.lookup_x = s_front_buffer.upscale_lookup_x;
    g_subframe_info.lookup_y = s_front_buffer.upscale_lookup_y;
    g_subframe_info.offset_x = s_front_buffer.upscale_offset_x;
    g_subframe_info.offset_y = s_front_buffer.upscale_offset_y;
    g_subframe_info.scaled_w = s_front_buffer.upscale_scaled_w;
    g_subframe_info.scaled_h = s_front_buffer.upscale_scaled_h;
    g_subframe_info.has_borders = s_front_buffer.upscale_has_borders;
    g_subframe_info.dst_w = s_front_buffer.upscale_dst_w;
    g_subframe_info.dst_h = s_front_buffer.upscale_dst_h;
#if CONFIG_P3A_PPA_UPSCALE_ENABLE
    g_subframe_info.use_ppa = (s_front_buffer.channel_type == PS_CHANNEL_TYPE_GIPHY);
#else
    g_subframe_info.use_ppa = false;
#endif
    g_subframe_info.valid = true;
}
```

**Note on native buffer pointer safety**: During the sub-frame loop, the render task never calls the callback, so the decoder never advances and the native buffer pointer remains valid. The loader task can only trigger a prefetch through the callback (which isn't being called), so no swap can invalidate the pointer during sub-framing.

## Step 4: Sub-frame loop in `display_render_task()`

### 4a: Reset sub-frame info before callback

In `display_render_task()`, right before calling the frame callback (~line 581), add:

```c
g_subframe_info.valid = false;  // Reset before each callback invocation
```

This ensures stale data from a previous animation frame doesn't leak into a non-animation frame (e.g. if p3a_render shows a channel message instead of calling the animation path).

### 4b: Sub-frame eligibility check and loop

After the callback returns and sets `g_target_frame_delay_ms` (line 602), BEFORE the existing overlay section (line 610), insert the sub-frame path. The existing overlay/timing/DMA sections become the `else` branch (single-frame path, unchanged).

Structure:

```c
// Check sub-frame eligibility
const bool subframe_eligible = !ui_mode
    && frame_delay_ms > DISPLAY_MAX_FRAME_DURATION_MS
    && g_subframe_info.valid
    && !config_store_get_max_speed_playback();

if (subframe_eligible) {
    // --- SUB-FRAME PATH ---
    const int sf_count = (frame_delay_ms + DISPLAY_MAX_FRAME_DURATION_MS - 1)
                       / DISPLAY_MAX_FRAME_DURATION_MS;
    const int64_t total_us = (int64_t)frame_delay_ms * 1000;

    // Guard: if decode+upscale already consumed more than one sub-frame's
    // worth of time, sub-framing would slow playback (each extra sub-frame
    // adds ~8-16ms VSYNC wait + ~5-10ms re-upscale overhead that can't be
    // hidden in the sleep time). Fall through to single-frame path.
    const int64_t callback_duration_us = esp_timer_get_time() - frame_processing_start_us;
    const int64_t sf_duration_us = total_us / sf_count;

    if (callback_duration_us < sf_duration_us) {

    // === Sub-frame 0: already decoded+upscaled by the callback ===

    // Draw overlays
    fps_update_and_draw(back_buffer);
    processing_notification_update_and_draw(back_buffer);

    // Wait for PREVIOUS frame's timing (identical to current single-frame logic)
    {
        const int64_t now_us = esp_timer_get_time();
        const int64_t target_delay_us = (int64_t)prev_frame_delay_ms * 1000;
        int64_t elapsed_us = (g_last_frame_present_us > 0)
            ? (now_us - g_last_frame_present_us)
            : (now_us - frame_processing_start_us);
        if (elapsed_us < target_delay_us) {
            int64_t residual_us = target_delay_us - elapsed_us;
            if (residual_us > 2000) {
                vTaskDelay(pdMS_TO_TICKS((residual_us + 500) / 1000));
            }
        }
    }

    // Submit sub-frame 0 to DMA
    g_last_display_buffer = (uint8_t)back_buffer_idx;
    if (use_triple_buffering) {
        xSemaphoreTake(g_display_vsync_sem, 0);
        xSemaphoreTake(g_display_vsync_sem, portMAX_DELAY);
        g_buffer_info[back_buffer_idx].state = BUFFER_STATE_PENDING;
        g_last_submitted_idx = back_buffer_idx;
    } else {
        g_render_buffer_index = ((uint8_t)back_buffer_idx + 1) % buffer_count;
    }
    esp_lcd_panel_draw_bitmap(g_display_panel, 0, 0,
                              EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, back_buffer);
    const int64_t sf0_present_us = esp_timer_get_time();
    g_last_frame_present_us = sf0_present_us;
    if (!use_vsync) vTaskDelay(5);

    // === Sub-frames 1..N-1: re-upscale from cached native buffer ===

    for (int sf = 1; sf < sf_count; sf++) {
        // Break conditions
        if (g_display_mode_request != DISPLAY_RENDER_MODE_ANIMATION) break;
        if (app_lcd_is_animation_paused()) break;
        if (app_lcd_get_brightness() == 0) break;

        // Acquire buffer
        if (use_triple_buffering) {
            back_buffer_idx = acquire_free_buffer(portMAX_DELAY);
            if (back_buffer_idx < 0) break;
            back_buffer = g_display_buffers[back_buffer_idx];
        } else {
            back_buffer_idx = (int8_t)g_render_buffer_index;
            back_buffer = g_display_buffers[back_buffer_idx];
        }
        if (!back_buffer) {
            if (use_triple_buffering && back_buffer_idx >= 0) {
                g_buffer_info[back_buffer_idx].state = BUFFER_STATE_FREE;
                if (g_buffer_free_sem) xSemaphoreGive(g_buffer_free_sem);
            }
            break;
        }

        // Re-upscale from cached native buffer
        subframe_reupscale(&g_subframe_info, back_buffer);

        // Draw overlays
        fps_update_and_draw(back_buffer);
        processing_notification_update_and_draw(back_buffer);

        // Wait until absolute target time (prevents drift)
        int64_t target_us = sf0_present_us + (sf * total_us) / sf_count;
        int64_t now_us = esp_timer_get_time();
        if (now_us < target_us) {
            int64_t residual_us = target_us - now_us;
            if (residual_us > 2000) {
                vTaskDelay(pdMS_TO_TICKS((residual_us + 500) / 1000));
            }
        }

        // Submit to DMA
        g_last_display_buffer = (uint8_t)back_buffer_idx;
        if (use_triple_buffering) {
            xSemaphoreTake(g_display_vsync_sem, 0);
            xSemaphoreTake(g_display_vsync_sem, portMAX_DELAY);
            g_buffer_info[back_buffer_idx].state = BUFFER_STATE_PENDING;
            g_last_submitted_idx = back_buffer_idx;
        } else {
            g_render_buffer_index = ((uint8_t)back_buffer_idx + 1) % buffer_count;
        }
        esp_lcd_panel_draw_bitmap(g_display_panel, 0, 0,
                                  EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, back_buffer);
        g_last_frame_present_us = esp_timer_get_time();
        if (!use_vsync) vTaskDelay(5);
    }

    // Set target delay for next iteration: remaining time of the last sub-frame.
    // This ensures the NEXT animation frame's timing starts correctly.
    // Formula: total_duration - time_consumed_by_first_N-1_subframes
    uint32_t elapsed_sf_ms = (uint32_t)(((int64_t)(sf_count - 1) * frame_delay_ms) / sf_count);
    g_target_frame_delay_ms = (uint32_t)frame_delay_ms - elapsed_sf_ms;

    continue;  // Skip single-frame path below

    } // end of callback_duration_us < sf_duration_us guard
    // If guard failed (decode too slow), fall through to single-frame path below.
}

// --- SINGLE-FRAME PATH (existing code, unchanged) ---
// 3. Apply overlays
fps_update_and_draw(back_buffer);
// ... etc
```

### Key timing detail: `g_target_frame_delay_ms` after sub-frame loop

The last sub-frame (N-1) was presented at approximately `sf0 + ((N-1)/N) * D`. The next animation frame should start at `sf0 + D`. The gap is `D/N`. Setting `g_target_frame_delay_ms = D - ((N-1)*D)/N` (which equals `D/N` with correct integer rounding) ensures the next iteration waits exactly the right amount after `g_last_frame_present_us`.

Example: 300ms frame, 3 sub-frames → last SF presented at ~T+200ms → `g_target_frame_delay_ms = 300 - 200 = 100ms` → next frame waits 100ms from last SF present → starts at ~T+300ms. Correct.

## Edge Cases

| Scenario | Behavior |
|----------|----------|
| Mode switch mid-sub-frame | Break condition at top of sf>0 loop |
| Pause mid-sub-frame | Break condition at top of sf>0 loop |
| Brightness=0 mid-sub-frame | Break condition at top of sf>0 loop |
| Rotation change mid-loop | CPU: uses cached lookup tables (stale but consistent, rebuilt on next callback). PPA: reads g_screen_rotation live (immediately correct). Acceptable. |
| Swap becomes ready | Can't happen — prefetch runs in callback context, which isn't called during sub-frame loop. Swap handled on next iteration. |
| PICO-8 / ugfx_ui / channel messages | `g_subframe_info.valid` is false (reset before callback, only set by animation path). Single-frame path used. |
| Static image, small delay | `frame_delay_ms <= 125` → single-frame path. No sub-framing. |
| Static image, large delay | Sub-framing activates correctly (re-upscale from cached b1). |
| Decode too slow for sub-frames | Guard: if callback duration ≥ sub-frame duration, skip sub-framing. Prevents VSYNC+re-upscale overhead from slowing playback when decode already consumes most of the frame time. |
| max_speed_playback | Excluded from eligibility. Frames submitted as fast as decoded. |
| frame_delay = 126ms | 2 sub-frames × 63ms each (truly even split). |
| frame_delay ≤ 125ms | Single-frame path (no sub-framing needed). |

## Verification

1. **Build**: `idf.py build` — must compile without errors
2. **Manual test with long-frame animation**: Load a GIF with frame delays > 125ms. Verify:
   - Processing notification appears within ~125ms of a swap command
   - FPS counter shows 8+ FPS even with slow animations
   - Animation timing looks correct (no speedup/slowdown)
3. **Manual test with fast animation**: Load a GIF with frame delays < 125ms. Verify no behavioral change (single-frame path used).
4. **Manual test with Giphy content**: Verify PPA upscale path works correctly for sub-frames.
5. **Manual test with static image**: Verify no unnecessary sub-framing when no overlay is active.
