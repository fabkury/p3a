# Sub-Frame Rendering Spec

## 1. Motivation

p3a's current graphics pipeline ties its screen update rate 1:1 to the underlying animation frame rate. The loop is:

1. Display frame F1.
2. Decode and upscale frame F2 while F1 is displayed.
3. Sleep for the remainder of F1's target duration ("frame delay").
4. Display F2. Repeat.

This works well when animation frames are short, but creates a problem when they are long. Consider an animation whose frames last 1 minute each. If a user triggers a swap command at T=00:15, p3a cannot show any visual feedback (e.g. a processing indicator) until T=01:00 when the next frame is due. The user waits 45 seconds with no on-screen acknowledgement.

More generally, p3a needs the ability to update the screen independently of the underlying animation's frame rate, for purposes such as:

- Showing processing indicators promptly.
- Animating overlays (e.g. animated emoji, progress bars, motion effects).
- In the future, applying per-frame visual effects to the upscaled image (movement, fading, transitions).

## 2. Goals

1. **Minimum 8 screen frames per second** (maximum 125 ms per screen frame), regardless of the underlying animation frame duration.
2. **Preserve animation frame timing accuracy.** The total display duration of each animation frame must match its target duration as closely as possible.
3. **Decode each native animation frame exactly once.** When a native frame must be displayed for longer than 125 ms, p3a decodes and composites it once in native resolution, then re-upscales the cached native frame for each sub-frame.
4. **Upscale on every screen frame.** Even when the native frame hasn't changed, the upscaling step runs every sub-frame. This enables future per-frame effects (movement, fading, transitions) without further architectural changes.
5. **Draw overlays after upscaling.** Overlays (processing notification, FPS counter, future animated emoji) are drawn into the display-resolution buffer after upscaling and before DMA submission. This keeps the native frame buffer clean for re-upscaling and ensures overlays render at display resolution.

## 3. Current Pipeline (Before)

Key files and their roles:

| File | Role |
|------|------|
| `main/display_renderer.c` | Core render task, triple buffering, VSYNC, DMA submission |
| `main/animation_player_render.c` | Frame decoding, upscaling, prefetch, buffer swap |
| `main/animation_player_loader.c` | Asset loading, decoder setup, upscale map building |
| `main/animation_player.c` | Initialization, swap requests, render dispatch |
| `main/display_upscaler.c` | CPU nearest-neighbor upscaling with rotation |
| `main/display_ppa_upscaler.c` | PPA hardware bilinear upscaling (Giphy) |
| `main/playback_controller.c` | Render source switching (animation, PICO-8, UI) |
| `main/display_renderer_priv.h` | Buffer states, overlay drawing, processing notification |

### Current render loop (`display_render_task`)

```
loop:
  acquire free buffer (triple buffering)
  call frame callback:
    decode next native frame   ← one decode per animation frame
    upscale to display buffer  ← one upscale per animation frame
  draw overlays (FPS, processing notification)
  sleep for frame_delay - elapsed
  submit buffer to DMA
```

Each iteration produces exactly one screen frame for one animation frame. The sleep duration equals the animation frame's delay minus decode+upscale time.

### Current frame timing

```c
target_delay_us = prev_frame_delay_ms * 1000;
elapsed_us = now - last_frame_present_us;
if (elapsed_us < target_delay_us) {
    residual = target_delay_us - elapsed_us;
    if (residual > 2000) vTaskDelay(residual_ms);
}
```

Straightforward single-sleep approach. No sub-frame concept.

## 4. Design: Sub-Frame Rendering

### 4.1 Core Concept

When an animation frame's target duration exceeds 125 ms, p3a splits it into multiple **sub-frames**, each displayed for an equal share of the total duration. The native frame is decoded once; each sub-frame re-upscales from the cached native buffer.

**Example:** A 300 ms animation frame becomes 3 sub-frames of 100 ms each (not 2 x 125 ms + 1 x 50 ms).

### 4.2 Sub-Frame Count and Duration Calculation

```
sub_frame_count = ceil(frame_delay_ms / MAX_FRAME_DURATION_MS)
sub_frame_duration_ms = frame_delay_ms / sub_frame_count
```

Where `MAX_FRAME_DURATION_MS = 125`.

This "truly even" split avoids pathologically short sub-frames. For example, a 126 ms frame becomes 2 x 63 ms (not 1 x 125 ms + 1 x 1 ms).

### 4.3 Drift Prevention with Absolute Timestamps

To prevent accumulated timing drift across sub-frames and across animation frames, each sub-frame targets an **absolute timestamp** rather than a relative delay.

```
anim_frame_start = timestamp when this animation frame began displaying
sub_frame_target[i] = anim_frame_start + (frame_delay_ms * (i + 1)) / sub_frame_count
```

Integer arithmetic with the multiply-then-divide order avoids floating-point imprecision and distributes any remainder evenly across sub-frames.

### 4.4 Revised Render Loop (Conceptual)

```
loop:
  if need_new_animation_frame:
    decode native frame into native buffer
    compute sub_frame_count and sub_frame_targets
    current_sub_frame = 0
    need_new_animation_frame = false

  acquire free buffer
  upscale native buffer → display buffer     ← every sub-frame
  draw overlays into display buffer           ← every sub-frame
  sleep until sub_frame_target[current_sub_frame]
  submit buffer to DMA

  current_sub_frame++
  if current_sub_frame >= sub_frame_count:
    need_new_animation_frame = true
```

### 4.5 What Happens on Each Sub-Frame

| Step | First sub-frame | Subsequent sub-frames |
|------|----------------|----------------------|
| Decode native frame | Yes | No (reuse cached) |
| Upscale to display | Yes | Yes (from cached native) |
| Draw overlays | Yes | Yes |
| Submit to DMA | Yes | Yes |

### 4.6 Upscaling

Both upscaling paths (CPU and PPA) run on every sub-frame:

- **CPU upscaler** (`display_upscaler.c`): Nearest-neighbor with lookup tables. Re-reads from cached native buffer each sub-frame.
- **PPA upscaler** (`display_ppa_upscaler.c`): Hardware-accelerated bilinear. Uses `PPA_TRANS_MODE_BLOCKING` — the call blocks until the hardware finishes, then returns a fully written display buffer. Overlays can be drawn into the buffer immediately after.

Running upscaling on every sub-frame (even when the native frame is unchanged) enables future per-frame visual effects without further architectural changes.

### 4.7 Static Images

Single-frame (static) images keep their current "decode once, upscale once, reuse" fast path **when no overlay is active and no effect is running**. When an overlay becomes active (e.g. processing notification triggered, animated emoji displayed), the static image switches to the sub-frame loop so the overlay can animate at 8+ fps. When the overlay deactivates, it can return to the fast path.

### 4.8 Overlays

Overlays are drawn into the display-resolution buffer after upscaling and before DMA submission. This is unchanged from the current architecture.

Overlay animation state is **time-driven** (wall-clock based), not tick-driven. Each overlay tracks its own absolute timestamps and interpolates its visual state based on the current time at each sub-frame. This decouples overlay animation smoothness from sub-frame timing variance (caused by upscaling time differences, RTOS scheduling jitter, etc.).

### 4.9 Buffer Swaps Between Sub-Frames

When a buffer swap (front/back) becomes ready mid-animation-frame (i.e. between sub-frames), the swap is **allowed immediately** at the next sub-frame boundary. The remaining sub-frames for the interrupted animation frame are discarded.

Rationale: if the user requested a swap, there is no visual benefit to finishing the current animation frame's remaining sub-frames. Swapping immediately gives better responsiveness.

### 4.10 Latency Characteristics

With 125 ms maximum sub-frame duration:
- Worst-case latency from event (e.g. swap request) to first visual feedback: **125 ms**. This is the time until the next sub-frame renders and draws the overlay.
- This latency is acceptable. No forced-wake mechanism is needed for the initial implementation.

## 5. Constraints

- **Computational speed is limited.** Some animations are heavy enough that p3a can barely decode them at their target speed. The sub-frame model must not add decode overhead — native frames are decoded exactly once.
- **Memory is constrained.** The native frame ping-pong buffers (b1/b2) and display triple buffers already consume significant RAM. No additional large buffers should be needed.
- **PPA scale quantization.** The PPA rounds scale factors to 1/16 increments, which can produce slightly different output dimensions than the CPU upscaler. This is an existing behavior and does not change with sub-frames.

## 6. Non-Goals (Out of Scope)

- Per-frame visual effects (movement, fading, transitions). The sub-frame architecture enables these but they are not implemented in this work.
- Animated overlay content (animated emoji, etc.). The sub-frame architecture provides the tick rate these need, but the overlay content itself is future work.
- Forced-wake for sub-125ms response. The 125 ms worst-case latency is acceptable.
- Higher minimum frame rates (e.g. 30 fps). 8 fps is sufficient for overlay animation and responsiveness.

## 7. Summary of Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Sub-frame splitting | Truly even (`ceil` then divide) | Avoids pathologically short sub-frames |
| Timing model | Absolute timestamps per sub-frame | Prevents drift accumulation |
| Upscaling frequency | Every sub-frame | Enables future per-frame effects |
| Native decode frequency | Once per animation frame | No unnecessary decode overhead |
| Overlay drawing | After upscaling, before DMA | Clean native buffer, display-resolution overlays |
| Overlay animation model | Time-driven (wall-clock) | Decoupled from sub-frame jitter |
| Static image behavior | Fast path when no overlay active | Avoids wasting CPU when nothing changes |
| Buffer swap timing | Allowed between sub-frames | Better swap responsiveness |
| Max screen frame duration | 125 ms (8 fps minimum) | Acceptable latency for visual feedback |
| Event-driven wake | Not needed | 125 ms worst-case is acceptable |
| PPA on sub-frames | Yes, same as CPU path | Blocking mode; overlays draw after return |
