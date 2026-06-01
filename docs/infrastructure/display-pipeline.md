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
so playback is decoupled from the refresh grid using a virtual playhead with fractional vsync
alignment.

### Producer / consumer split

Frame production and presentation run in **two tasks** (both pinned to core 1), connected by the
small `g_ready_queue`:

- **`display_producer_task`** acquires a `FREE` framebuffer, renders into it — the mode branch:
  the animation frame callback, UI via `ugfx_ui_render_to_buffer`, or a black frame when
  paused/brightness-zero — **bakes the overlays** (FPS, processing/reaction/pin), then enqueues a
  `ready_frame_t { buffer_idx, duration_ms, generation }`. It blocks in `acquire_free_buffer()`
  on `g_buffer_free_sem` once every buffer is downstream, so it is **back-pressured to the present
  rate** and runs at most ~1 frame ahead with 3 buffers.
- **`display_consumer_task`** dequeues a ready frame, drops it if its generation is stale (see
  flush-on-change), paces it to the virtual playhead, phase-locks to the vsync grid, and submits
  with `esp_lcd_panel_draw_bitmap`. It owns all timing state (`s_target_present_us`,
  `g_last_frame_present_us`) and runs at priority +1 so it preempts the heavy producer to hit
  vsync deadlines.

Because the producer banks a finished frame ahead, a single frame whose decode+upscale overruns
its interval can still be presented on time from the queue — the spike absorption that the
previous single-loop "render → sleep → present" design could not provide.

### VSYNC delivery

VSYNC is interrupt-driven, not polled:

1. `prepare_vsync()` registers `display_panel_refresh_done_cb` as the panel's `on_refresh_done`
   callback and creates a **binary semaphore** (`g_display_vsync_sem`).
2. On each refresh-done interrupt the callback:
   - **Stamps the edge**: `g_last_vsync_us = esp_timer_get_time()` — the exact moment the vsync
     fired. The consumer reads this to align submits and to tell a *fresh* signal from a
     *cached* one sitting in the binary semaphore.
   - **Advances buffer ownership** (see triple buffering below): the `PENDING` buffer becomes
     `DISPLAYING`, the previously-displaying buffer is freed and its slot returned via
     `g_buffer_free_sem`.
   - **Signals** the consumer by giving the vsync semaphore.

### Virtual playhead (consumer)

`s_target_present_us` is a file-scope accumulator representing *when the current frame should
appear*. Each dequeued frame carries its own intended `duration_ms`, snapshotted by the producer
at decode time — by present time the decoder has raced ahead, so the consumer can't query it.
Per frame the consumer:

- Presents at `s_target_present_us`, then **after submit** advances the playhead by *this*
  frame's carried duration (`s_target_present_us += duration_ms * 1000`). Duration accumulates
  exactly once per presented frame, so **long-term drift is zero** — the average frame rate
  matches the source even though individual frames don't land on vsync boundaries.
- Sleeps (`vTaskDelay`) until ~1.5 ms before the target, leaving slack for the alignment loop.
  Trivial residuals (<3 ms) skip the sleep and just block on the vsync sem.
- The **alignment loop** takes a vsync signal, reads the ISR-stamped `g_last_vsync_us`, and
  presents on the vsync edge *nearest* the playhead: if `vsync_us + half_period >= target`, this
  edge is the closest one → submit here; otherwise wait for the next. The half-period rounding
  gives fractional alignment — 50 fps content alternates 16.67 / 33.33 ms presentations to
  **average** 20 ms, instead of ceiling-quantizing every frame to 33.33 ms.

The accumulator advances by each frame's carried duration *after* it is submitted, so each queue
entry owns its dwell time; this replaced the single-loop design's `prev_frame_delay_ms`
bookkeeping (which advanced by the duration of the frame being replaced).

### Flush-on-change (content generation)

`g_render_generation` is bumped via `display_renderer_note_content_discontinuity()` whenever the
on-screen content should change immediately: a **mode switch** or **pause/brightness transition**
(detected by the producer) or an **artwork swap** (signalled from `animation_player_render.c`
after the front/back flip). The producer stamps each queued frame with the generation current
when it finished; the consumer **drops** any dequeued frame whose generation is stale (returning
the buffer straight to `FREE`) and baselines the playhead on the first fresh-generation frame.
This lands swaps and mode switches on the next vsync instead of after the ~1–2 queued frames
drain, with no stale frame ever presented.

### Drift safeguards

- **Positive-drift resync** (`FRAME_TIMING_RESYNC_US`, 250 ms): if wall clock runs more than
  250 ms *ahead* of the playhead (a slow decode, SD-I/O burst, or preemption stole that much
  time), re-baseline the playhead to "now" rather than submitting a burst of catch-up frames
  that would visibly fast-forward the animation. **Negative** drift (target ahead of now) is
  the *normal* state for any frame longer than one vsync period and is deliberately **not**
  resynced — doing so was the original bug that pinned everything to 60 fps.
- **Stale cached-vsync guard** (`cached_too_late`): the binary semaphore may hold a signal that
  fired earlier. If the consumed signal's timestamp is more than half a period old, "now" is
  already into the next refresh and submitting would tear, so the loop skips it and waits for a
  fresh vsync (the accumulator compensates next frame).

### Timing edge cases

- **Max-speed playback** (`config_store_get_max_speed_playback()`): the consumer bypasses the
  playhead and sleep entirely — consumes one vsync per submit, running at the full 60 Hz panel
  rate regardless of source durations.
- **First frame** after init / discontinuity / long pause (`g_last_frame_present_us == 0`):
  baselines the playhead to "now" so it presents immediately.
- **Paused / brightness-zero**: the producer outputs a black frame with a fixed 100 ms delay and
  bumps the generation on the transition.
- **Static images**: decoded once and cached; the frame callback returns the cached delay each
  tick without re-decoding (re-compositing only when the background color changes on a
  transparent asset).
- **Callback error (returns -1)**: the producer releases the buffer without enqueuing; DMA keeps
  showing the last submitted frame — no flicker.

### Triple buffering

Buffers move through `FREE → RENDERING → READY → PENDING → DISPLAYING` and back to `FREE`
(`buffer_state_t`). The producer acquires a `FREE` buffer (blocking on `g_buffer_free_sem`),
renders into it, marks it `READY` and enqueues it; the consumer marks it `PENDING` at submit
time; and the vsync ISR promotes `PENDING → DISPLAYING` while freeing the buffer DMA just
released. With 3 buffers the producer banks ~1 finished frame ahead of the one on screen, so
decode/upscale latency on a single frame is hidden instead of stalling scan-out. `g_ready_queue`
carries finished frames from producer to consumer; `g_buffer_free_sem` is the back-pressure
channel (FREE buffers are produced only by the ISR).

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
