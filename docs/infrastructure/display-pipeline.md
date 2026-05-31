# Display Pipeline

## Initialization Flow

1. **Board init**: `p3a_board_display_init()` via BSP — configures MIPI-DSI, ST7703 controller
2. **Frame buffers**: Allocated in PSRAM (720x720x3 bytes each for RGB888, triple-buffered)
3. **Display renderer**: `display_renderer_init()` sets up vsync, buffer management

## Rendering Pipeline

```
┌──────────────┐     ┌────────────────┐     ┌──────────────┐
│   Decoder    │────>│ animation_     │────>│ Upscaler     │
│ (WebP/GIF/   │     │ player_render  │     │ (CPU or PPA) │
│  PNG/JPEG)   │     │ (decode,       │     └──────┬───────┘
└──────────────┘     │  upscale       │            │
                     │  routing)      │            ▼
                     └────────────────┘     ┌─────────────┐
                                            │ display_    │
                                            │ renderer    │
┌──────────────────────────┐                │ (vsync,     │
│ Overlays                 │───────────────>│  buffers)   │
│ - FPS counter            │                └──────┬──────┘
│ - Processing notification│                       │
└──────────────────────────┘                       ▼
                                            ┌─────────────┐
                                            │ LCD Panel   │
                                            │ DMA (DSI)   │
                                            └─────────────┘
```

## Frame Timing and VSYNC

The panel runs at a fixed **60 Hz** refresh (`ST7703_720_720_PANEL_60HZ_DPI_CONFIG`), so one
refresh ≈ 16.667 µs (`VSYNC_PERIOD_US` in `display_renderer.c`). Animation frame delays
(e.g. a 50 fps WebP wants 20 ms/frame) are usually **not** integer multiples of that period,
so the render task decouples *playback speed* from the *refresh grid* using a virtual playhead
with fractional vsync alignment. All of this lives in `display_render_task()`.

### VSYNC delivery

VSYNC is interrupt-driven, not polled:

1. `prepare_vsync()` registers `display_panel_refresh_done_cb` as the panel's `on_refresh_done`
   callback and creates a **binary semaphore** (`g_display_vsync_sem`).
2. On each refresh-done interrupt the callback:
   - **Stamps the edge**: `g_last_vsync_us = esp_timer_get_time()` — the exact moment the vsync
     fired. The render task reads this to align submits and to tell a *fresh* signal from a
     *cached* one sitting in the binary semaphore.
   - **Advances buffer ownership** (see triple buffering below): the `PENDING` buffer becomes
     `DISPLAYING`, the previously-displaying buffer is freed and its slot returned via
     `g_buffer_free_sem`.
   - **Signals** the render task by giving the vsync semaphore.

### Virtual playhead

`s_target_present_us` is a file-scope accumulator representing *when the next frame should
appear*. Each iteration:

- The playhead advances by **exactly the previous frame's intended duration**
  (`s_target_present_us += prev_frame_delay_ms * 1000`). This is the only place source-side
  timing accumulates, so **long-term drift is zero** — the average frame rate matches the
  source even though individual frames don't land on vsync boundaries.
- The task sleeps (`vTaskDelay`) until ~1.5 ms before the target, leaving slack for the
  alignment loop. Trivial residuals (<3 ms) skip the sleep and just block on the vsync sem.
- The **alignment loop** then takes a vsync signal, reads the ISR-stamped `g_last_vsync_us`,
  and presents on the vsync edge *nearest* the playhead: if `vsync_us + half_period >= target`,
  this edge is the closest one → submit here; otherwise wait for the next. The half-period
  rounding is what gives fractional alignment — 50 fps content alternates 16.67 / 33.33 ms
  presentations to **average** 20 ms, instead of ceiling-quantizing every frame to 33.33 ms.

This replaced a legacy "sleep for `target - elapsed`, then wait for one fresh vsync" approach
that ceiling-quantized every frame to a multiple of the vsync period and lost up to a full
extra vsync per frame, visibly slowing animations.

### Drift safeguards

- **Positive-drift resync** (`FRAME_TIMING_RESYNC_US`, 250 ms): if wall clock runs more than
  250 ms *ahead* of the playhead (a slow decode, SD-I/O burst, or preemption stole that much
  time), re-baseline the playhead to "now" rather than submitting a burst of catch-up frames
  that would visibly fast-forward the animation. **Negative** drift (target ahead of now) is
  the *normal* state for any frame longer than one loop iteration and is deliberately **not**
  resynced — doing so was the original bug that pinned everything to 60 fps.
- **Stale cached-vsync guard** (`cached_too_late`): the binary semaphore may hold a signal that
  fired earlier. If the consumed signal's timestamp is more than half a period old, "now" is
  already into the next refresh and submitting would tear, so the loop skips it and waits for a
  fresh vsync (the accumulator compensates next frame).

### Timing edge cases

- **Max-speed playback** (`config_store_get_max_speed_playback()`): bypasses the playhead and
  sleep entirely — consumes one vsync per submit, running at the full 60 Hz panel rate
  regardless of source durations.
- **First frame** after init / mode change / long pause (`g_last_frame_present_us == 0`):
  baselines the playhead to "now" so it presents immediately.
- **Paused / brightness-zero**: outputs a black frame with a fixed 100 ms delay.
- **Static images**: decoded once and cached; the frame callback returns the cached delay each
  tick without re-decoding (re-compositing only when the background color changes on a
  transparent asset).
- **Callback error (returns -1)**: the back buffer is released and DMA keeps showing the last
  submitted frame — no flicker.

### Triple buffering

Buffers move through `FREE → RENDERING → PENDING → DISPLAYING` and back to `FREE`
(`buffer_state_t`). The render task acquires a `FREE` buffer (blocking on `g_buffer_free_sem`),
renders into it, marks it `PENDING` at submit time, and the vsync ISR promotes `PENDING →
DISPLAYING` while freeing the buffer DMA just released. This lets the task decode/upscale frame
N+1 while DMA scans frame N and a third frame waits, so decode latency doesn't stall scan-out.

## Rendering Features

- **Transparency/alpha blending**: Images with transparent backgrounds are composited over a configurable background color
- **Aspect ratio preservation**: Non-square images are scaled to fit while maintaining original proportions
- **Configurable background**: Background color can be set via web UI, REST API, or `render_engine_set_background()`
- **Channel-aware upscaling**: Giphy content uses PPA hardware-accelerated bilinear interpolation (smooth results for photographic/video content), while pixel art from Makapix and local files uses CPU nearest-neighbor scaling (crisp pixel edges).
- **Rotation**: 0/90/180/270 degree rotation, handled in both CPU and PPA upscale paths

## Upscaling Paths

### CPU Nearest-Neighbor (`display_upscaler.c`)
- Parallel upscaling using two FreeRTOS worker tasks (top/bottom halves)
- Handles rotation (0/90/180/270)
- Fills border regions with background color
- Cache coherency handling for PSRAM buffers

### PPA Hardware Bilinear (`display_ppa_upscaler.c`)
- Uses ESP32-P4 PPA (Pixel Processing Accelerator) SRM engine
- Bilinear interpolation for smooth upscaling
- Handles rotation and R/B channel swap in hardware
- Border regions filled via PPA Fill operation
- Conditional compilation: `CONFIG_P3A_PPA_UPSCALE_ENABLE`

## Overlays

### FPS Counter (`display_fps_overlay.c`)
- Renders FPS counter in the top-right corner using a 5x7 bitmap font (2x scaled)
- Tracks frame count and timing, displays up to 3 digits
- Respects config store enable/disable setting

### Processing Notification (`display_processing_notification.c`)
- Visual indicator for animation swap processing (blue checkerboard triangle) and failure (red triangle)
- State machine: IDLE → PROCESSING → FAILED → IDLE
- Drawn in the bottom-right corner, configurable size (16-256 px)
- 5-second timeout for processing, 3-second display for failure

## Key Files

| File | Purpose |
|------|---------|
| `display_renderer.c` | Frame buffer management, vsync, buffer lifecycle, virtual-playhead frame timing (see "Frame Timing and VSYNC") |
| `display_upscaler.c` | Parallel CPU nearest-neighbor upscaling with rotation and border fill |
| `display_ppa_upscaler.c` | PPA SRM bilinear upscaling with rotation (conditional) |
| `display_fps_overlay.c` | FPS counter overlay |
| `display_processing_notification.c` | Swap processing/failure visual indicator |
| `render_engine.c` | Display rotation and background color API |
| `animation_player_render.c` | Frame decoding and PPA/CPU upscale routing (aspect-ratio maps are built in `animation_player_loader.c`) |
| `animation_player_loader.c` | Aspect-ratio-preserving upscale map building, frame decode setup |
| `playback_controller.c` | Source switching between animation and PICO-8 (UI mode is gated separately by `display_renderer_enter_ui_mode()`) |
