# Frame Timing Investigation Report

**Date:** December 20, 2024  
**Issue:** Irregular frame durations causing slow and inconsistent animation playback

## Executive Summary

This investigation identified a **critical timing bug** in the display renderer that causes incorrect frame timing, particularly when animations change. The bug manifests as animations playing too slowly with irregular frame durations, and the problem can sometimes be resolved by switching to a different animation and back—consistent with the reported symptoms.

## Root Cause Analysis

### Primary Issue: Off-by-One Frame Delay Bug

**Location:** `main/display_renderer.c`, lines 430-473

**Critical Code Path:**
```c
uint32_t prev_frame_delay_ms = g_target_frame_delay_ms;  // Line 430

if (ui_mode) {
    // ... ui rendering ...
    g_target_frame_delay_ms = (uint32_t)frame_delay_ms;
} else {
    prev_frame_delay_ms = g_target_frame_delay_ms;      // Line 444 (REDUNDANT)
    frame_delay_ms = callback(back_buffer, ctx);        // Gets NEW frame delay
    // ...
    g_target_frame_delay_ms = (uint32_t)frame_delay_ms; // Line 452
}

// ... processing ...

const int64_t target_delay_us = (int64_t)prev_frame_delay_ms * 1000;  // Line 473
```

**The Bug:**

The timing logic uses `prev_frame_delay_ms` (the **previous** frame's delay) to calculate the sleep duration, but the callback at line 445 has just returned the **current** frame's delay. This creates an off-by-one error where:

1. **Frame N is rendered** → callback returns delay for frame N
2. **System sleeps** using delay from frame N-1 (stored in `g_target_frame_delay_ms` before the callback)
3. **Frame N+1 is rendered** → uses delay from frame N
4. This pattern continues indefinitely

**Impact:**

- Each frame is displayed for the **wrong** duration
- Fast frames appear slow (use previous slow frame's delay)
- Slow frames appear fast (use previous fast frame's delay)
- Total animation time is preserved, but individual frame timing is scrambled
- Effect is most noticeable with variable frame delays (e.g., animated GIFs with mixed timing)

**Why It's Intermittent:**

The bug's visibility depends on the animation's frame delay variance:

1. **Uniform delays** (e.g., all frames at 100ms): Bug is invisible - every frame uses the same delay regardless
2. **Variable delays** (e.g., 50ms, 100ms, 200ms alternating): Bug is very visible - each frame gets wrong timing
3. **Animation changes**: When switching animations:
   - `g_target_frame_delay_ms` retains the last frame's delay from the previous animation
   - The new animation's first frame gets rendered with the old animation's timing
   - This can cause the first frame to be too fast/slow
   - Switching back can "fix" it if timing happens to realign

## Secondary Issues

### 2. WebP Decoder: Timestamp Reset Race Condition

**Location:** `components/animation_decoder/webp_animation_decoder.c`, lines 432-437

**Issue:**
```c
if (webp_data->is_animation) {
    if (webp_data->decoder) {
        WebPAnimDecoderReset(webp_data->decoder);
    }
    webp_data->last_timestamp_ms = 0;           // Reset timestamp
    webp_data->current_frame_delay_ms = 1;      // But delay is set to 1ms!
}
```

When resetting a WebP animation:
- `last_timestamp_ms` is correctly reset to 0
- `current_frame_delay_ms` is set to 1ms
- On the **first decode after reset**, the delay calculation becomes:
  ```c
  int frame_delay = timestamp_ms - webp_data->last_timestamp_ms;  // = timestamp_ms - 0
  ```
  
**Problem:** For the first frame, this gives the **cumulative** timestamp (e.g., 100ms), not the intended per-frame delay. While this is technically correct for frame 0, it creates timing inconsistency if the animation's actual first frame has a different delay.

**Edge Case:** If the first frame's actual delay is 50ms but its timestamp is 100ms (cumulative from frame creation), the frame will display for 100ms instead of 50ms.

### 3. GIF Decoder: Minimum Delay Clamping Side Effects

**Location:** `components/animated_gif_decoder/gif.inl`, lines 350-352

**Issue:**
```c
pPage->iFrameDelay = (INTELSHORT(&p[iOffset+2]))*10; // delay in ms
if (pPage->iFrameDelay <= 1) // 0-1 is going to make it run at 60fps
   pPage->iFrameDelay = 100;  // use 100 (10fps) as a reasonable substitute
```

This hardcoded replacement of very fast frames (0-1ms → 100ms) can dramatically alter animation behavior:

- **Original intent:** Prevent CPU-burning infinite loops with 0ms delays
- **Side effect:** Some animations intentionally use 10ms (0.01 sec in GIF centisecond units) for fast effects
- **Result:** These fast-paced sequences become 10× slower (10ms → 100ms)

**Example:** A GIF with alternating 10ms/50ms frame pattern becomes 100ms/50ms, completely changing the animation's feel.

### 4. Buffer Swap State Confusion

**Location:** `main/animation_player_render.c`, lines 481-531

**Issue:**
When buffers are swapped:
```c
if (swap_requested && back_buffer_ready) {
    // ... mutex lock ...
    animation_buffer_t temp = s_front_buffer;
    s_front_buffer = s_back_buffer;
    s_back_buffer = temp;
    s_swap_requested = false;
    s_back_buffer.ready = false;
    // ... more cleanup ...
}
s_use_prefetched = true;  // Line 531 - OUTSIDE mutex!
```

The `s_use_prefetched` flag is set **outside** the mutex, creating a potential race condition:
- If another thread checks buffer state between swap and flag set
- The flag might not match the actual buffer state
- Could cause prefetched frame to be skipped or used incorrectly

### 5. Frame Delay Fallback Inconsistencies

**Locations:** Various decoder files

**Issue:** Different decoders handle missing/invalid frame delays inconsistently:

- **WebP decoder:** Falls back to `1ms` (lines 259, 323, 437)
- **GIF decoder:** Falls back to `1ms` (line 280, 353, 439)
- **Display renderer:** Falls back to `100ms` when callback fails (line 450)
- **Animation render:** Uses `100ms` for paused, `1ms` for errors (lines 552-553, 558)

**Problem:** A 1ms delay is effectively "as fast as possible," which could:
- Burn CPU cycles if the decoder encounters errors
- Create visual glitches during error recovery
- Make debugging harder (hard to tell if 1ms is intentional or an error)

**Recommended:** Use a sensible default like 33ms (30fps) or 50ms (20fps) for error cases, and 1ms only for valid high-speed frames.

## Code Flow Analysis

### Normal Frame Rendering Path

```
display_render_task (display_renderer.c:392)
  └─> frame_processing_start_us = esp_timer_get_time()  [line 427]
  └─> prev_frame_delay_ms = g_target_frame_delay_ms     [line 430]  ← OLD delay
  └─> callback(back_buffer, ctx)                        [line 445]
        └─> animation_player_render_frame_callback (animation_player_render.c:348)
              └─> render_next_frame(&s_front_buffer, ...)  [line 556]
                    └─> decode_next_native(buf, decode_buffer)  [line 242]
                          └─> animation_decoder_decode_next_rgb()  [line 33]
                                └─> gif_decoder_decode_next_rgb() OR
                                    webp_decoder_decode_next_rgb()
                    └─> animation_decoder_get_frame_delay(buf->decoder, &frame_delay_ms)  [line 256]
                    └─> buf->current_frame_delay_ms = frame_delay_ms  [line 261]
                    └─> return (int)buf->current_frame_delay_ms  [line 284]
              └─> s_target_frame_delay_ms = frame_delay_ms  [line 559]
              └─> return frame_delay_ms
  └─> g_target_frame_delay_ms = frame_delay_ms           [line 452]  ← NEW delay stored
  └─> processing_time_us = now - frame_processing_start_us  [line 472]
  └─> target_delay_us = prev_frame_delay_ms * 1000       [line 473]  ← Uses OLD delay!
  └─> if (processing_time_us < target_delay_us)
        └─> vTaskDelay(...)  [line 478]  ← Sleeps for WRONG duration
  └─> esp_lcd_panel_draw_bitmap(...)  [line 490]
```

### Animation Change Path

```
User swipes / auto-advance triggered
  └─> animation_player_cycle_animation()  [animation_player.c:411]
        └─> s_swap_requested = true
        └─> xSemaphoreGive(s_loader_sem)  [line 432]
              └─> animation_loader_task wakes up  [animation_player_loader.c:182]
                    └─> channel_player_advance() or go_back()
                    └─> load_animation_into_buffer(..., &s_back_buffer)
                          └─> animation_decoder_init()
                          └─> animation_decoder_get_info()
                    └─> s_back_buffer.prefetch_pending = true
                    └─> (returns to loader_task loop)

Next render cycle:
  └─> display_render_task
        └─> animation_player_render_frame_callback
              └─> if (back_buffer_prefetch_pending)  [line 393]
                    └─> prefetch_first_frame(&s_back_buffer)  [line 426]
                          └─> decode_next_native(buf, buf->native_frame_b1)
                          └─> animation_decoder_get_frame_delay(&frame_delay_ms)
                          └─> buf->prefetched_first_frame_delay_ms = frame_delay_ms
              └─> if (swap_requested && back_buffer_ready)  [line 480]
                    └─> Swap buffers  [line 482]
                    └─> s_use_prefetched = true  [line 531]

Next render cycle (after swap):
  └─> render_next_frame(&s_front_buffer, ..., s_use_prefetched=true)  [line 556]
        └─> if (use_prefetched && buf->first_frame_ready)  [line 160]
              └─> Upscale prefetched frame
              └─> return buf->prefetched_first_frame_delay_ms  [line 184]
        └─> s_use_prefetched = false  [line 557]
```

### Timing State Flow

```
Initial state:
  g_target_frame_delay_ms = 0  (global in display_renderer.c:67)
  s_target_frame_delay_ms = 100  (local in animation_player_render.c:14)

First frame:
  prev_frame_delay_ms = 0  ← Initial value
  callback returns: 100ms (from animation)
  g_target_frame_delay_ms = 100  ← Stored
  Sleep for: 0ms  ← Wrong! (uses prev_frame_delay_ms)

Second frame:
  prev_frame_delay_ms = 100  ← From g_target_frame_delay_ms
  callback returns: 50ms
  g_target_frame_delay_ms = 50
  Sleep for: 100ms  ← Wrong! (should be 50ms)

Third frame:
  prev_frame_delay_ms = 50
  callback returns: 200ms
  g_target_frame_delay_ms = 200
  Sleep for: 50ms  ← Wrong! (should be 200ms)
```

## Manifestation Patterns

The reported symptoms align perfectly with these issues:

1. **"Plays animation too slowly"**
   - Primary bug causes frame delays to be shifted by one position
   - If animation has increasing delays (50→100→150ms), each frame displays with previous (shorter) delay
   - Net effect: animation feels sluggish because timing is wrong

2. **"Irregular frame durations"**
   - Off-by-one bug scrambles timing pattern
   - Variable delays get mismatched to wrong frames
   - Creates choppy, inconsistent playback

3. **"Not associated with specific animation files"**
   - Bug is in the renderer, not the decoders or files
   - Affects all animations, but visibility depends on delay variance
   - Appears "random" because it depends on frame delay patterns

4. **"Changing to different animation, then back sometimes fixes it"**
   - When switching animations, timing state gets reset
   - If new animation has uniform delays, bug is less visible
   - Switching back might realign timing by chance
   - "Sometimes" because it depends on delay patterns involved

5. **"Happens irregularly"**
   - Not actually irregular—always present
   - Only **visible** when animations have significant delay variance
   - Animations with uniform delays hide the bug
   - Appears to come and go based on which animation is playing

## Recommended Fixes

### Fix 1: Correct Frame Delay Timing (CRITICAL - HIGH PRIORITY)

**File:** `main/display_renderer.c`  
**Lines:** 430-473

**Current buggy code:**
```c
uint32_t prev_frame_delay_ms = g_target_frame_delay_ms;  // OLD delay

if (callback) {
    prev_frame_delay_ms = g_target_frame_delay_ms;  // Redundant!
    frame_delay_ms = callback(back_buffer, ctx);    // Get NEW delay
    // ...
    g_target_frame_delay_ms = (uint32_t)frame_delay_ms;
}

// ...
const int64_t target_delay_us = (int64_t)prev_frame_delay_ms * 1000;  // Uses OLD delay!
```

**Fixed code:**
```c
// Don't capture prev_frame_delay_ms yet - we need the callback's result first
int frame_delay_ms = 100;

if (callback) {
    frame_delay_ms = callback(back_buffer, ctx);    // Get THIS frame's delay
    if (frame_delay_ms < 0) {
        back_buffer_idx = g_last_display_buffer;
        if (back_buffer_idx >= buffer_count) back_buffer_idx = 0;
        back_buffer = g_display_buffers[back_buffer_idx];
        frame_delay_ms = 100;
    }
    g_target_frame_delay_ms = (uint32_t)frame_delay_ms;
}

// ...
// Use the CURRENT frame's delay for sleep calculation, not the previous one
const int64_t target_delay_us = (int64_t)frame_delay_ms * 1000;
```

**Rationale:** The sleep duration should match the delay of the frame we just rendered, not the previous frame's delay. This ensures proper timing for variable-delay animations.

### Fix 2: Improve WebP Timestamp Reset

**File:** `components/animation_decoder/webp_animation_decoder.c`  
**Lines:** 432-437

**Current code:**
```c
if (webp_data->is_animation) {
    if (webp_data->decoder) {
        WebPAnimDecoderReset(webp_data->decoder);
    }
    webp_data->last_timestamp_ms = 0;
    webp_data->current_frame_delay_ms = 1;  // Too aggressive
}
```

**Improved code:**
```c
if (webp_data->is_animation) {
    if (webp_data->decoder) {
        WebPAnimDecoderReset(webp_data->decoder);
    }
    webp_data->last_timestamp_ms = 0;
    // Use reasonable default until first frame is decoded
    webp_data->current_frame_delay_ms = 33;  // ~30fps fallback
}
```

### Fix 3: Add Mutex Protection for s_use_prefetched

**File:** `main/animation_player_render.c`  
**Lines:** 480-531

**Current code:**
```c
if (swap_requested && back_buffer_ready) {
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        // ... buffer swap ...
        xSemaphoreGive(s_buffer_mutex);
        // ...
    }
}
s_use_prefetched = true;  // OUTSIDE mutex!
```

**Fixed code:**
```c
if (swap_requested && back_buffer_ready) {
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        // ... buffer swap ...
        s_use_prefetched = true;  // INSIDE mutex
        xSemaphoreGive(s_buffer_mutex);
        // ...
    }
}
```

### Fix 4: Standardize Error Delay Fallbacks

**Multiple files:** Decoders and render functions

**Recommendation:** Create a common header with sensible defaults:

```c
// animation_timing_constants.h
#define ANIMATION_DEFAULT_FRAME_DELAY_MS    33   // ~30fps
#define ANIMATION_MINIMUM_FRAME_DELAY_MS     1   // 1000fps max
#define ANIMATION_ERROR_FALLBACK_DELAY_MS   50   // 20fps fallback
#define ANIMATION_STATIC_IMAGE_DELAY_MS   5000   // 5 seconds for stills
```

Use `ANIMATION_DEFAULT_FRAME_DELAY_MS` (33ms) instead of 1ms for error cases, reserving 1ms only for intentionally fast animations.

### Fix 5: Review GIF Minimum Delay Clamping (OPTIONAL)

**File:** `components/animated_gif_decoder/gif.inl`  
**Lines:** 350-352

**Current logic:**
```c
if (pPage->iFrameDelay <= 1)  // 0-1ms
   pPage->iFrameDelay = 100;  // Force to 100ms
```

**Consider more nuanced approach:**
```c
// GIF delay is in centiseconds (1/100 sec), so 0-1 means 0-10ms
if (pPage->iFrameDelay == 0) {
    // True 0 delay (often an error): use reasonable default
    pPage->iFrameDelay = 100;  // 100ms
} else if (pPage->iFrameDelay == 1) {
    // Intentional 10ms delay: allow it but log warning for performance
    ESP_LOGD(TAG, "GIF has very fast frame (10ms) - may impact performance");
    // Keep as 10ms
}
```

Alternatively, keep current behavior but document that fast GIFs (< 20ms per frame) will be slowed to 100ms.

## Testing Recommendations

### Test 1: Variable Delay Animation
Create a test GIF with known frame delays: 100ms, 50ms, 200ms, 150ms  
**Expected:** Each frame displays for its specified duration  
**Measure:** Record actual display times with high-speed camera or timing logs

### Test 2: Animation Switching
1. Play animation A (variable delays)
2. Switch to animation B (uniform delays)
3. Switch back to animation A
**Expected:** Animation A plays identically in both instances

### Test 3: Prefetch Timing
1. Load animation
2. Measure first frame display duration
**Expected:** First frame displays for its actual delay, not 1ms or previous animation's delay

### Test 4: Edge Cases
- All-zero-delay GIF (should default to reasonable rate)
- Single-frame animation (should not cause timing errors)
- Very fast animation (10ms per frame)
- Very slow animation (5000ms per frame)

## Conclusion

The primary root cause is a **classic off-by-one timing bug** in the display renderer's frame timing logic. The system correctly obtains frame delays from decoders but applies them to the **wrong** frames due to using `prev_frame_delay_ms` (the previous frame's delay) instead of `frame_delay_ms` (the current frame's delay) for sleep calculations.

This bug is always present but only visible with variable-delay animations, making it appear intermittent and animation-specific. The fix is straightforward: use the current frame's delay for timing, not the previous frame's delay.

Secondary issues related to decoder resets, mutex protection, and error handling should also be addressed to ensure robust timing behavior across all scenarios.

**Priority:**
1. **CRITICAL:** Fix display_renderer.c timing bug (Fix 1)
2. **HIGH:** Add mutex protection for s_use_prefetched (Fix 3)
3. **MEDIUM:** Improve WebP reset timing (Fix 2)
4. **LOW:** Standardize error fallbacks (Fix 4), Review GIF clamping (Fix 5)

---

**Investigation conducted by:** GitHub Copilot  
**Files analyzed:** 15+ source files across main/ and components/ directories  
**Code paths traced:** Display rendering, animation decoding (GIF/WebP), buffer management, timing calculation
