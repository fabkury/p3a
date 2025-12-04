# Screen Rotation Implementation Guide

## Executive Summary

This document analyzes different approaches to implementing screen rotation (90°, 180°, 270°) for the p3a pixel art player, with emphasis on runtime changeability and minimal performance impact. The device uses a 720×720 square IPS display with an ESP32-P4 microcontroller running efficient multi-buffered animation playback.

## Current Architecture Overview

### Display Pipeline

The p3a uses a sophisticated display pipeline optimized for pixel art animation playback:

1. **Resolution**: 720×720 pixels (square display)
2. **Buffer Configuration**: 2-3 frame buffers for smooth vsync rendering
3. **Rendering Flow**:
   - Decoder (WebP/GIF/PNG/JPEG) → Native frame buffer (source resolution)
   - Upscale with lookup tables → LCD frame buffer (720×720)
   - DPI panel → Display

### Key Components

- **`animation_player_render.c`**: Core rendering loop (`lcd_animation_task`)
- **`blit_webp_frame_rows()`**: Upscales decoded frames using pre-computed lookup tables
- **Upscale workers**: Two parallel tasks split rendering (top/bottom halves)
- **Frame buffers**: Direct MIPI-DSI DPI panel buffers
- **LCD driver**: `esp_lcd_mipi_dsi` with `esp_lcd_dpi_panel`

### Current Upscaling Method

The system uses pre-computed X/Y lookup tables for nearest-neighbor upscaling:
```c
// Generated once per animation load
buf->upscale_lookup_x[dst_x] = (dst_x * canvas_w) / target_w;
buf->upscale_lookup_y[dst_y] = (dst_y * canvas_h) / target_h;
```

Rendering copies source pixels row-by-row:
```c
for (int dst_y = row_start; dst_y < row_end; ++dst_y) {
    const uint16_t src_y = lookup_y[dst_y];
    const uint8_t *src_row = src_rgba + (size_t)src_y * src_w * 4;
    
    for (int dst_x = 0; dst_x < dst_w; ++dst_x) {
        const uint16_t src_x = lookup_x[dst_x];
        const uint8_t *pixel = src_row + (size_t)src_x * 4;
        dst_row[dst_x] = rgb565(pixel[0], pixel[1], pixel[2]);
    }
}
```

## Rotation Implementation Approaches

### Approach 1: Rotation in Upscale Lookup Tables (RECOMMENDED)

**Description**: Modify the lookup table generation to encode rotation transformations. The rendering loop remains unchanged.

**Implementation Details**:

```c
// Current: Identity mapping
for (int dst_x = 0; dst_x < target_w; ++dst_x) {
    buf->upscale_lookup_x[dst_x] = (dst_x * canvas_w) / target_w;
}

// 90° CW rotation: (x, y) → (y, width - 1 - x)
for (int dst_x = 0; dst_x < target_w; ++dst_x) {
    int rotated_x = dst_x; // becomes y in source
    buf->upscale_lookup_x[dst_x] = (rotated_x * canvas_h) / target_w;
}
for (int dst_y = 0; dst_y < target_h; ++dst_y) {
    int rotated_y = target_w - 1 - dst_y; // becomes (width - 1 - x)
    buf->upscale_lookup_y[dst_y] = (rotated_y * canvas_w) / target_h;
}

// 180° rotation: (x, y) → (width - 1 - x, height - 1 - y)
for (int dst_x = 0; dst_x < target_w; ++dst_x) {
    int rotated_x = target_w - 1 - dst_x;
    buf->upscale_lookup_x[dst_x] = (rotated_x * canvas_w) / target_w;
}

// 270° CW rotation: (x, y) → (height - 1 - y, x)
// Similar logic to 90° but reversed
```

**Changes Required**:
1. Add rotation parameter to `load_animation_into_buffer()` and related functions
2. Modify lookup table generation in `animation_player_loader.c` (~50 lines)
3. Add runtime rotation state variable and API functions
4. Store rotation preference in NVS config

**Runtime Rotation**:
- Regenerate lookup tables when rotation changes
- Only affects back buffer (next animation)
- Current animation finishes at old rotation
- Swap happens smoothly within 1-2 frames

**Pros**:
- ✅ Zero rendering overhead - lookup tables are pre-computed
- ✅ No changes to hot rendering path (`blit_webp_frame_rows`)
- ✅ Works with existing multi-core upscale workers unchanged
- ✅ Minimal memory overhead (lookup tables already exist)
- ✅ Runtime rotation via regenerating lookup tables for next animation
- ✅ Simple implementation (~100 lines total)
- ✅ Compatible with all pixel formats (RGB565, RGB888)
- ✅ Works for all animation types (WebP, GIF, PNG, JPEG)

**Cons**:
- ⚠️ Rotation applies to next animation (1-2 frame latency)
- ⚠️ For non-square source images, aspect handling needs consideration
- ⚠️ Source coordinate to rotated coordinate mapping requires careful math

**Performance Impact**: **Negligible**
- Lookup table generation: ~0.1ms one-time cost per animation load
- Per-frame rendering: 0% overhead (same memory access pattern)
- CPU usage: Unchanged
- Memory: No additional allocation

---

### Approach 2: Hardware Display Rotation (If Available)

**Description**: Use MIPI-DSI panel's built-in rotation capabilities via commands like `MADCTL` (Memory Access Control).

**Implementation Details**:

```c
// Panel command to set rotation
esp_err_t set_panel_rotation(uint8_t rotation) {
    uint8_t madctl_value = 0x00;
    switch (rotation) {
        case 90:  madctl_value = 0x60; break; // MV | MX
        case 180: madctl_value = 0xC0; break; // MY | MX
        case 270: madctl_value = 0xA0; break; // MV | MY
        default:  madctl_value = 0x00; break; // Normal
    }
    return esp_lcd_panel_io_tx_param(io_handle, 0x36, &madctl_value, 1);
}
```

**Pros**:
- ✅ Zero software overhead - handled by display controller
- ✅ Instant runtime rotation (sub-millisecond)
- ✅ No code changes to rendering pipeline
- ✅ Applies to current frame immediately
- ✅ Works for all content including UI, animations, and PICO-8 mode

**Cons**:
- ❌ **Hardware dependent** - may not be supported by this panel
- ❌ Requires panel documentation for correct register values
- ❌ Limited to 90° increments (typically)
- ❌ May have panel-specific quirks or limitations
- ❌ Testing needed to verify support

**Applicability**: 
**Unknown** - Requires checking the Waveshare ESP32-P4-WIFI6-Touch-LCD-4B panel specifications. The panel uses MIPI-DSI but may or may not expose rotation registers. The BSP (Board Support Package) would need to be checked for existing rotation support.

**Performance Impact**: **None** (if supported)

---

### Approach 3: Post-Render Rotation Transform

**Description**: Rotate the frame buffer after rendering but before display.

**Implementation Details**:

```c
void rotate_buffer_90cw(uint8_t *src, uint8_t *dst, int w, int h, size_t stride) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            // (x, y) → (y, w-1-x)
            int dst_x = y;
            int dst_y = w - 1 - x;
            uint16_t *src_pixel = (uint16_t*)(src + y * stride + x * 2);
            uint16_t *dst_pixel = (uint16_t*)(dst + dst_y * stride + dst_x * 2);
            *dst_pixel = *src_pixel;
        }
    }
}
```

**Changes Required**:
1. Allocate additional rotation buffer (720×720 × 2 bytes = ~1MB)
2. Add rotation function between rendering and display
3. Modify `lcd_animation_task` to call rotation function

**Pros**:
- ✅ Immediate runtime rotation (applies to current frame)
- ✅ Independent of source content or format
- ✅ Can optimize with SIMD/DMA for ESP32-P4

**Cons**:
- ❌ **High memory cost**: Requires extra full-frame buffer (~1MB)
- ❌ **Performance overhead**: 720×720 pixel copy per frame (~5-10ms)
- ❌ Cache thrashing from non-sequential memory access
- ❌ Reduces effective frame rate capability
- ❌ Complex optimization needed for acceptable performance

**Performance Impact**: **High**
- Per-frame overhead: ~5-10ms (naive implementation)
- Memory: +1MB (7% increase)
- Potential for frame drops on complex animations

---

### Approach 4: Decode-Time Rotation

**Description**: Rotate during frame decode by modifying decoder output directly.

**Implementation Details**:

Would require:
1. Modifying each decoder (WebP, GIF, PNG, JPEG) output
2. Decoder-specific rotation logic
3. Native buffer dimensions swap for 90°/270° rotations

**Pros**:
- ✅ No extra rotation pass
- ✅ Can be efficient if decoders support it natively

**Cons**:
- ❌ **Complex**: Requires changes to 4 different decoders
- ❌ **Decoder dependencies**: libwebp, AnimatedGIF, etc. may not support rotation
- ❌ **Maintainability**: Decoder updates could break rotation
- ❌ Native buffer size changes complicate memory management
- ❌ Aspect ratio issues with non-square sources

**Performance Impact**: **Variable** (depends on decoder capabilities)

---

### Approach 5: Touch Coordinate Transformation

**Description**: Software rotation is paired with touch coordinate transformation to maintain correct interaction.

**Implementation Details**:

```c
void transform_touch_coordinates(int *x, int *y, uint8_t rotation) {
    int temp;
    switch (rotation) {
        case 90:  // CW
            temp = *x;
            *x = *y;
            *y = SCREEN_HEIGHT - 1 - temp;
            break;
        case 180:
            *x = SCREEN_WIDTH - 1 - *x;
            *y = SCREEN_HEIGHT - 1 - *y;
            break;
        case 270: // CCW
            temp = *x;
            *x = SCREEN_WIDTH - 1 - *y;
            *y = temp;
            break;
    }
}
```

**Note**: This is a **companion technique** required for any software rotation approach. Touch input must be transformed to match the rotated display.

**Changes Required**:
1. Add transformation function in `app_touch.c`
2. Apply before gesture recognition
3. ~20 lines of code

---

## Recommendations

### Primary Recommendation: **Approach 1 (Lookup Table Rotation)**

**Why**:
1. **Zero performance impact**: No per-frame overhead
2. **Simple implementation**: Minimal code changes (~100 lines)
3. **Memory efficient**: Uses existing lookup table infrastructure
4. **Maintainable**: Self-contained logic in one function
5. **Compatible**: Works with all animation formats and UI rendering
6. **Runtime capable**: Can change rotation between animations

**Trade-off**: 1-2 frame latency when changing rotation (acceptable for user-initiated changes)

### Secondary Recommendation: **Investigate Approach 2 (Hardware Rotation)**

**Action**: Check panel datasheet and BSP for MADCTL or similar rotation support
- If available: Use as primary method (instant, zero overhead)
- If not: Fall back to Approach 1

**Why investigate**:
- Would provide instant rotation with zero overhead
- Simple to implement if supported
- Best user experience

### Implementation Priority

1. **Phase 1**: Implement Approach 1 (lookup table rotation)
   - Add rotation API to animation_player.h
   - Modify lookup table generation
   - Add NVS config storage
   - Add touch coordinate transformation
   - Estimated effort: 4-6 hours

2. **Phase 2**: Investigate hardware rotation (Approach 2)
   - Review panel documentation
   - Test MADCTL commands
   - If successful, make primary method
   - Estimated effort: 2-4 hours

3. **Phase 3**: Add user interface
   - Web API endpoint for rotation setting
   - Touch gesture for rotation (e.g., 3-finger swipe)
   - Config UI in web interface
   - Estimated effort: 2-3 hours

### Not Recommended

- **Approach 3 (Post-render rotation)**: Too expensive in memory and performance
- **Approach 4 (Decode-time rotation)**: Too complex, high maintenance burden

---

## Implementation Checklist

### Core Rotation System
- [ ] Define rotation enum (0°, 90°, 180°, 270°)
- [ ] Add global rotation state variable
- [ ] Implement lookup table rotation logic
- [ ] Modify `load_animation_into_buffer()` to use rotation
- [ ] Add touch coordinate transformation
- [ ] Store/load rotation preference in NVS

### API Functions
- [ ] `animation_player_set_rotation(rotation_t angle)`
- [ ] `animation_player_get_rotation()`
- [ ] Trigger back buffer reload on rotation change

### Configuration
- [ ] Add rotation config to `config_store` component
- [ ] Persist across reboots
- [ ] Default: 0° (no rotation)

### User Interface
- [ ] Web API endpoint: `GET/POST /api/rotation`
- [ ] Add rotation setting to web UI
- [ ] Optional: Touch gesture for quick rotation toggle

### Testing
- [ ] Test all 4 rotation angles
- [ ] Verify touch coordinates match display
- [ ] Test with various aspect ratio source images
- [ ] Performance validation (should be ~0ms overhead)
- [ ] Memory leak testing on repeated rotation changes

---

## Technical Specifications

### Memory Requirements
- Lookup tables: 2 × 720 × 2 bytes = 2.8KB per animation (already allocated)
- Config storage: ~4 bytes (rotation angle)
- Total: **Negligible increase**

### Performance Targets
- Rotation switch time: < 100ms (lookup table regeneration)
- Per-frame overhead: 0ms
- Animation load time increase: < 0.1ms
- Frame rate: Unchanged (60 FPS capable)

### Compatibility Matrix

| Feature | 0° | 90° | 180° | 270° | Notes |
|---------|----|----|-----|------|-------|
| WebP Animation | ✓ | ✓ | ✓ | ✓ | Full support |
| GIF Animation | ✓ | ✓ | ✓ | ✓ | Full support |
| PNG/JPEG | ✓ | ✓ | ✓ | ✓ | Full support |
| UI Rendering (µGFX) | ✓ | ⚠️ | ⚠️ | ⚠️ | Needs separate handling |
| PICO-8 Stream | ✓ | ⚠️ | ⚠️ | ⚠️ | Needs separate handling |
| Touch Input | ✓ | ✓ | ✓ | ✓ | With coordinate transform |

⚠️ = Requires additional implementation for µGFX and PICO-8 rendering paths

---

## Conclusion

The **lookup table rotation approach (Approach 1)** provides the optimal balance of:
- **Performance**: Zero per-frame overhead
- **Simplicity**: Minimal code changes, easy to maintain
- **Memory efficiency**: No additional buffers required
- **Runtime flexibility**: Can change rotation during operation
- **Compatibility**: Works with entire rendering pipeline

This approach aligns perfectly with p3a's core function as a pixel art player where accurate frame timing is paramount. The implementation is straightforward, well-isolated, and introduces no risk to the existing optimized rendering pipeline.

**Next Steps**: 
1. Implement lookup table rotation (Approach 1)
2. Test with all animation formats
3. Add user-facing controls (web API, touch gestures)
4. Optionally investigate hardware rotation as future optimization

---

## Appendix: Code Locations

### Key Files for Implementation

1. **`main/animation_player_loader.c`**
   - Modify lookup table generation (~lines 405-419)
   - Add rotation parameter handling

2. **`main/animation_player_priv.h`**
   - Add rotation enum and state variable
   - Declare rotation functions

3. **`main/animation_player.c`**
   - Add public rotation API functions
   - Handle rotation change requests

4. **`main/app_touch.c`**
   - Add touch coordinate transformation
   - Apply before gesture processing

5. **`components/config_store/`**
   - Add rotation config persistence

6. **`components/http_api/`**
   - Add rotation endpoint

### Estimated Total LOC: ~200 lines
- Core rotation: ~100 lines
- Touch transform: ~20 lines
- Config/API: ~50 lines
- Testing/validation: ~30 lines
