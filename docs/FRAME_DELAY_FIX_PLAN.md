# Frame Delay Bug Fix Plan

## Problem Statement

The p3a pixel art animation player has a design error: **the frame delay of frame N is being applied to frame N+1**. This causes timing mismatches where each frame is displayed for the duration specified by the *previous* frame rather than its own delay.

## Root Cause Analysis

### Current Flow (Incorrect)

In `animation_player_render.c`, the `lcd_animation_task()` function processes frames in the following order:

1. **Record processing start time** (line 516): `s_frame_processing_start_us = esp_timer_get_time()`
2. **Save previous frame delay** (line 594): `prev_frame_delay_ms = s_target_frame_delay_ms`
3. **Decode and render frame N** (line 595): `render_next_frame()` returns `frame_delay_ms` for frame N
4. **Update target delay** (line 598): `s_target_frame_delay_ms = (uint32_t)frame_delay_ms` (delay for frame N)
5. **Wait using PREVIOUS frame's delay** (line 636): `target_delay_us = (int64_t)prev_frame_delay_ms * 1000`
6. **Display frame N** (line 656): `esp_lcd_panel_draw_bitmap()`

### The Bug

The timing wait (step 5) uses `prev_frame_delay_ms`, which was captured *before* decoding the current frame. This means:

- When displaying Frame 0, the delay used is whatever `s_target_frame_delay_ms` was initialized to (16 ms default)
- When displaying Frame 1, the delay used is Frame 0's delay
- When displaying Frame 2, the delay used is Frame 1's delay
- And so on...

This is a **one-frame lag** in delay application.

### Why This Design Exists

The current design attempts to account for processing time by:
1. Starting a timer before rendering
2. Using the previous frame's delay as the target
3. Subtracting processing time from the target delay

However, this approach is fundamentally flawed because it uses the *wrong frame's* delay.

## Proposed Fix

### Approach 1: Use Current Frame's Delay (Recommended)

Change the timing logic to use the current frame's delay instead of the previous frame's delay.

**Modified flow:**
1. Decode and render frame N → get `frame_delay_ms` for frame N
2. Calculate remaining delay: `remaining_delay = frame_delay_ms - processing_time`
3. Wait for remaining delay
4. Display frame N

**Code changes in `lcd_animation_task()` (animation_player_render.c):**

```c
// BEFORE (lines 594-598):
prev_frame_delay_ms = s_target_frame_delay_ms;
frame_delay_ms = render_next_frame(&s_front_buffer, back_buffer, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, use_prefetched);
use_prefetched = false;
if (frame_delay_ms < 0) frame_delay_ms = 1;
s_target_frame_delay_ms = (uint32_t)frame_delay_ms;

// AFTER:
// Remove prev_frame_delay_ms = s_target_frame_delay_ms;
frame_delay_ms = render_next_frame(&s_front_buffer, back_buffer, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, use_prefetched);
use_prefetched = false;
if (frame_delay_ms < 0) frame_delay_ms = 1;
s_target_frame_delay_ms = (uint32_t)frame_delay_ms;
```

```c
// BEFORE (line 636):
const int64_t target_delay_us = (int64_t)prev_frame_delay_ms * 1000;

// AFTER:
const int64_t target_delay_us = (int64_t)s_target_frame_delay_ms * 1000;
```

**Additional cleanup:**
- Remove the `prev_frame_delay_ms` variable declaration (line 519)
- Update initialization at line 519 if needed

### Approach 2: Pre-fetch Delay Before Decode

An alternative approach would be to fetch the frame delay *before* decoding, but this would require API changes to the decoder interface and is more invasive.

### Edge Cases to Consider

1. **First frame**: The first frame's delay should be applied correctly. With the fix, `s_target_frame_delay_ms` will be set by `render_next_frame()` and used immediately.

2. **Prefetched frames**: When using prefetched first frames (line 589), the `prefetched_first_frame_delay_ms` is returned and should be used correctly.

3. **Animation loop**: When the animation loops, the last frame's delay and first frame's delay should both be honored correctly.

4. **Paused state**: When paused, a fixed delay of 100 ms is used (line 592), which is intentional behavior.

5. **UI mode**: UI mode uses its own delay logic (line 525-531), which is unaffected.

6. **PICO-8 mode**: PICO-8 rendering (lines 580-586) handles its own timing, unaffected.

## Files to Modify

| File | Changes |
|------|---------|
| `main/animation_player_render.c` | Remove `prev_frame_delay_ms` variable; use `s_target_frame_delay_ms` for timing |

## Testing Strategy

1. **Visual Verification**: Play an animation with varying frame delays (e.g., a test WebP/GIF with frames of 100ms, 500ms, 100ms pattern) and verify each frame displays for its specified duration.

2. **Timing Instrumentation**: Enable `CONFIG_P3A_LCD_DISPLAY_FRAME_DURATIONS` to show frame timing on screen and verify it matches the animation file's frame delays.

3. **Animation Format Testing**: Test with:
   - Animated WebP files
   - Animated GIF files
   - Static images (should have fixed delay)

4. **Edge Case Testing**:
   - Single-frame animations
   - Very fast animations (< 20ms per frame)
   - Very slow animations (> 1000ms per frame)
   - Animations with inconsistent frame delays

## Impact Assessment

- **Risk**: Low - the change is localized to timing logic
- **Backward Compatibility**: This is a bug fix; correct behavior is expected
- **Performance**: No impact - timing calculation remains the same complexity

## Implementation Notes

1. The `prev_frame_delay_ms` variable at line 519 can be removed entirely after the fix.

2. The `s_target_frame_delay_ms` global is used for:
   - Frame timing in the render loop
   - Display of frame duration text (when debug mode is enabled)
   
   Both uses are internal to the render loop, so the fix is self-contained.

3. The timing calculation at lines 634-644 compensates for processing time, which is correct behavior. Only the delay source needs to change.

## Summary

The fix requires changing **2-3 lines of code** in `animation_player_render.c`:
1. Remove the line that saves `prev_frame_delay_ms`
2. Change the timing calculation to use `s_target_frame_delay_ms` instead of `prev_frame_delay_ms`
3. (Optional) Remove the now-unused `prev_frame_delay_ms` variable declaration

This ensures each frame is displayed for its own specified delay, not the previous frame's delay.
