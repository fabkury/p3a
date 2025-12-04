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

## µGFX UI Rotation

### Overview

The µGFX library used for the provisioning/registration UI has **built-in software rotation support** that is already enabled in the current codebase. This is handled through the `GDISP_HARDWARE_CONTROL` feature and coordinate transformation in the framebuffer driver.

### How µGFX Rotation Works

**Current Implementation**:
- Driver: `components/ugfx/drivers/gdisp/framebuffer/gdisp_lld_framebuffer.c`
- Configuration: `GDISP_HARDWARE_CONTROL` is enabled (line 19 of `gdisp_lld_config.h`)
- Orientation support: Already implemented in `gdisp_lld_draw_pixel()` (lines 84-99)

**Rotation Mechanism**:
```c
// In gdisp_lld_draw_pixel() - coordinate transformation per orientation
switch(g->g.Orientation) {
case gOrientation0:   // No rotation
    pos = PIXIL_POS(g, g->p.x, g->p.y);
    break;
case gOrientation90:  // 90° CW
    pos = PIXIL_POS(g, g->p.y, g->g.Width-g->p.x-1);
    break;
case gOrientation180: // 180°
    pos = PIXIL_POS(g, g->g.Width-g->p.x-1, g->g.Height-g->p.y-1);
    break;
case gOrientation270: // 270° CW
    pos = PIXIL_POS(g, g->g.Height-g->p.y-1, g->p.x);
    break;
}
```

**Width/Height Swapping**:
The `gdisp_lld_control()` function automatically swaps width and height when rotating between portrait/landscape modes:
```c
case GDISP_CONTROL_ORIENTATION:
    if (g->g.Orientation == gOrientation90 || g->g.Orientation == gOrientation270) {
        // Swap dimensions for 90°/270° rotations
        gCoord tmp = g->g.Width;
        g->g.Width = g->g.Height;
        g->g.Height = tmp;
    }
    g->g.Orientation = (gOrientation)g->p.ptr;
```

### µGFX Rotation API

**Setting Orientation** (already available):
```c
// Use µGFX built-in control API
gdispControl(GDISP_CONTROL_ORIENTATION, (void*)gOrientation90);

// Or use the convenience function
gdispSetOrientation(gOrientation90);
```

**Orientation Enum**:
```c
typedef enum gOrientation {
    gOrientation0 = 0,        // 0° (native)
    gOrientation90 = 90,      // 90° CW
    gOrientation180 = 180,    // 180°
    gOrientation270 = 270,    // 270° CW
} gOrientation;
```

### Integration with Animation Rotation

**Approach 6: Unified Rotation System (RECOMMENDED)**

**Description**: Synchronize µGFX rotation with animation player rotation to provide consistent screen orientation across all display modes.

**Implementation Strategy**:

1. **Global Rotation State**:
```c
// In animation_player_priv.h
typedef enum {
    ROTATION_0   = 0,
    ROTATION_90  = 90,
    ROTATION_180 = 180,
    ROTATION_270 = 270
} screen_rotation_t;

extern screen_rotation_t g_screen_rotation;
```

2. **Apply to Animation Player** (Approach 1 - Lookup Tables):
```c
// Modify lookup table generation based on g_screen_rotation
esp_err_t set_animation_rotation(screen_rotation_t rotation) {
    g_screen_rotation = rotation;
    
    // Trigger back buffer reload with new lookup tables
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        s_swap_requested = true;
        xSemaphoreGive(s_buffer_mutex);
        xSemaphoreGive(s_loader_sem);
    }
    
    return ESP_OK;
}
```

3. **Apply to µGFX UI**:
```c
// In ugfx_ui.c - add rotation sync function
esp_err_t ugfx_ui_set_rotation(screen_rotation_t rotation) {
    gOrientation ugfx_orientation;
    
    switch (rotation) {
        case ROTATION_0:   ugfx_orientation = gOrientation0; break;
        case ROTATION_90:  ugfx_orientation = gOrientation90; break;
        case ROTATION_180: ugfx_orientation = gOrientation180; break;
        case ROTATION_270: ugfx_orientation = gOrientation270; break;
        default: return ESP_ERR_INVALID_ARG;
    }
    
    if (s_ugfx_initialized) {
        gdispSetOrientation(ugfx_orientation);
    }
    
    return ESP_OK;
}
```

4. **Unified API**:
```c
// Public API in animation_player.h
esp_err_t app_set_screen_rotation(screen_rotation_t rotation) {
    // Apply to animation player
    set_animation_rotation(rotation);
    
    // Apply to µGFX UI
    ugfx_ui_set_rotation(rotation);
    
    // Store in config
    config_store_set_rotation(rotation);
    
    return ESP_OK;
}
```

**Pros**:
- ✅ Consistent rotation across animation and UI modes
- ✅ Uses native µGFX rotation (already implemented)
- ✅ Single API call rotates entire display
- ✅ No additional performance overhead for UI
- ✅ Clean architecture - each subsystem handles its own rotation

**Cons**:
- ⚠️ Animation rotation has 1-2 frame latency (acceptable)
- ⚠️ Slight implementation effort to coordinate both systems (~50 lines)

**Performance Impact**:
- **Animation rendering**: 0% overhead (lookup table approach)
- **µGFX rendering**: Minimal overhead (coordinate transform per pixel, but UI is infrequent)
- **Total**: Negligible - UI renders only during provisioning/registration

### µGFX Rotation Performance Analysis

**Per-Pixel Cost**:
- 1 switch statement (~4-8 CPU cycles)
- 1-2 arithmetic operations (subtraction, no division)
- **Total**: ~10-15 CPU cycles per pixel

**Registration UI Frame**:
- Text rendering: ~50,000 pixels (sparse, mostly empty)
- Update frequency: 1 Hz (countdown timer)
- Total overhead: ~0.5ms per frame (negligible at 1 FPS UI)

**Why This Is Acceptable**:
1. UI rendering is **infrequent** (once per second for timer updates)
2. UI is **low complexity** (text only, no complex graphics)
3. UI has **no strict timing** (registration screen, not animation playback)
4. ESP32-P4 dual-core can handle the overhead easily

### Implementation Checklist for µGFX Integration

#### Core Changes
- [ ] Add global rotation state (`screen_rotation_t`)
- [ ] Create unified rotation API (`app_set_screen_rotation()`)
- [ ] Add `ugfx_ui_set_rotation()` function
- [ ] Initialize µGFX with correct orientation on startup
- [ ] Synchronize rotation between animation and UI subsystems

#### Configuration
- [ ] Load rotation from NVS on boot
- [ ] Apply to both animation player and µGFX
- [ ] Persist changes to NVS

#### Testing
- [ ] Test rotation in animation mode (all 4 angles)
- [ ] Test rotation in UI mode (registration screen)
- [ ] Verify smooth transition between animation ↔ UI modes
- [ ] Test rotation change while in UI mode
- [ ] Verify touch coordinates work in all rotations

#### Code Locations
1. **`main/ugfx_ui.c`**: Add `ugfx_ui_set_rotation()`
2. **`main/ugfx_ui.h`**: Expose rotation function
3. **`main/animation_player.c`**: Add unified API
4. **`main/animation_player.h`**: Public rotation functions
5. **`components/config_store/`**: Add rotation persistence

**Estimated Total LOC**: ~150 lines
- Animation rotation: ~100 lines
- µGFX integration: ~30 lines
- Unified API: ~20 lines

---

## Recommendations

### Primary Recommendation: **Approach 6 (Unified Rotation System)**

**Why**:
1. **Comprehensive**: Handles both animation and UI rotation
2. **Zero performance impact for animations**: Uses lookup table approach
3. **Minimal overhead for UI**: Uses µGFX built-in rotation (already implemented)
4. **Consistent UX**: Same rotation across all display modes
5. **Clean architecture**: Each subsystem manages its own rotation
6. **Simple API**: Single function call to rotate entire display

**What This Involves**:
- **Animation Player**: Lookup table rotation (Approach 1) - ~100 lines
- **µGFX UI**: Native rotation support (already exists) - ~30 lines integration
- **Unified API**: Coordinate both systems - ~20 lines
- **Touch Transform**: Required for all approaches - ~20 lines

**Trade-offs**: 
- Animation rotation has 1-2 frame latency (acceptable for user-initiated changes)
- µGFX has minimal per-pixel overhead (~10-15 cycles), but UI is infrequent (1 FPS)

### Secondary Recommendation: **Investigate Approach 2 (Hardware Rotation)**

**Action**: Check panel datasheet and BSP for MADCTL or similar rotation support
- If available: Use as primary method for instant rotation
- If not: Use Approach 6 (Unified System)

**Why investigate**:
- Would provide instant rotation with zero overhead for both animation and UI
- Simple to implement if supported
- Best user experience

**Note**: Hardware rotation would still need touch coordinate transformation

### Implementation Priority

1. **Phase 1**: Implement Unified Rotation System (Approach 6)
   - Add global rotation state and unified API
   - Implement animation lookup table rotation
   - Integrate µGFX rotation (call `gdispSetOrientation()`)
   - Add touch coordinate transformation
   - Add NVS config storage
   - Estimated effort: 5-7 hours

2. **Phase 2**: Investigate hardware rotation (Approach 2)
   - Review panel documentation
   - Test MADCTL commands
   - If successful, replace software rotation for instant changes
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
- [ ] Define rotation enum (`screen_rotation_t`: 0°, 90°, 180°, 270°)
- [ ] Add global rotation state variable
- [ ] Implement lookup table rotation logic for animations
- [ ] Modify `load_animation_into_buffer()` to use rotation
- [ ] Add touch coordinate transformation
- [ ] Store/load rotation preference in NVS

### Animation Player Integration
- [ ] `set_animation_rotation(screen_rotation_t angle)` - internal function
- [ ] Update lookup table generation with rotation transforms
- [ ] Trigger back buffer reload on rotation change

### µGFX UI Integration
- [ ] `ugfx_ui_set_rotation(screen_rotation_t angle)` - public function
- [ ] Map `screen_rotation_t` to `gOrientation` enum
- [ ] Call `gdispSetOrientation()` when rotation changes
- [ ] Handle rotation change during active UI session

### Unified API Functions
- [ ] `app_set_screen_rotation(screen_rotation_t angle)` - main public API
- [ ] `app_get_screen_rotation()` - query current rotation
- [ ] Coordinate animation player and µGFX UI rotation
- [ ] Apply rotation on system startup

### Configuration
- [ ] Add rotation config to `config_store` component
- [ ] Persist across reboots
- [ ] Default: 0° (no rotation)

### User Interface
- [ ] Web API endpoint: `GET/POST /api/rotation`
- [ ] Add rotation setting to web UI
- [ ] Optional: Touch gesture for quick rotation toggle

### Testing
- [ ] Test all 4 rotation angles in animation mode
- [ ] Test all 4 rotation angles in UI mode (registration screen)
- [ ] Test mode transitions (animation ↔ UI) with rotation active
- [ ] Verify touch coordinates match display in all rotations
- [ ] Test with various aspect ratio source images
- [ ] Performance validation (animations: ~0ms overhead, UI: negligible)
- [ ] Memory leak testing on repeated rotation changes
- [ ] Test rotation change while UI is active

---

## Technical Specifications

### Memory Requirements
- Lookup tables: 2 × 720 × 2 bytes = 2.8KB per animation (already allocated)
- µGFX rotation: 0 bytes (software transformation)
- Config storage: ~4 bytes (rotation angle)
- Total: **Negligible increase**

### Performance Targets
- Rotation switch time: < 100ms (lookup table regeneration + µGFX update)
- Per-frame overhead (animation): 0ms
- Per-frame overhead (UI): ~0.5ms (negligible at 1 FPS UI update rate)
- Animation load time increase: < 0.1ms
- Frame rate: Unchanged (60 FPS capable for animations, 1 FPS for UI)

### Compatibility Matrix

| Feature | 0° | 90° | 180° | 270° | Notes |
|---------|----|----|-----|------|-------|
| WebP Animation | ✓ | ✓ | ✓ | ✓ | Full support via lookup tables |
| GIF Animation | ✓ | ✓ | ✓ | ✓ | Full support via lookup tables |
| PNG/JPEG | ✓ | ✓ | ✓ | ✓ | Full support via lookup tables |
| UI Rendering (µGFX) | ✓ | ✓ | ✓ | ✓ | Native µGFX rotation support |
| PICO-8 Stream | ✓ | ⚠️ | ⚠️ | ⚠️ | Needs separate handling (future work) |
| Touch Input | ✓ | ✓ | ✓ | ✓ | With coordinate transform |

⚠️ = PICO-8 requires additional implementation (future work)

---

## Conclusion

The **Unified Rotation System (Approach 6)** provides comprehensive rotation support across all display modes:

### Key Benefits
- **Animations**: Zero per-frame overhead via lookup table rotation
- **UI**: Native µGFX rotation support (already implemented in driver)
- **Unified API**: Single function call to rotate entire display
- **Performance**: Negligible impact - animations at 0ms overhead, UI at ~0.5ms (only during infrequent updates)
- **Memory efficiency**: No additional buffers required
- **Runtime flexibility**: Can change rotation during operation
- **Clean architecture**: Each subsystem handles its own rotation independently

### Why This Works Well

1. **For Animations** (Primary Use Case):
   - Lookup table approach preserves accurate frame timing
   - Zero overhead maintains 60 FPS capability
   - Critical for pixel art playback accuracy

2. **For UI** (Provisioning/Registration):
   - µGFX rotation already implemented in framebuffer driver
   - Minimal overhead acceptable (UI renders at 1 FPS)
   - No complex graphics or tight timing requirements

3. **System Integration**:
   - Touch coordinates transformed consistently
   - Single rotation state across all modes
   - Smooth transitions between animation and UI

### Implementation Summary

**Total Effort**: ~150 lines of code, 5-7 hours
- Animation rotation: ~100 lines
- µGFX integration: ~30 lines  
- Unified API & config: ~20 lines

**Next Steps**: 
1. Implement unified rotation system (Approach 6)
2. Test across all display modes (animation, UI, mode transitions)
3. Add user-facing controls (web API, touch gestures)
4. Optionally investigate hardware rotation for instant rotation (Approach 2)

---

## Appendix: Code Locations

### Key Files for Implementation

#### Animation Player
1. **`main/animation_player_loader.c`**
   - Modify lookup table generation (~lines 405-419)
   - Add rotation parameter handling

2. **`main/animation_player_priv.h`**
   - Add `screen_rotation_t` enum
   - Add global rotation state variable
   - Declare internal rotation functions

3. **`main/animation_player.c`**
   - Add unified API: `app_set_screen_rotation()`
   - Add internal: `set_animation_rotation()`
   - Handle rotation change requests

#### µGFX UI Integration
4. **`main/ugfx_ui.c`**
   - Add `ugfx_ui_set_rotation()` function
   - Map rotation enum to µGFX `gOrientation`
   - Call `gdispSetOrientation()` when needed

5. **`main/ugfx_ui.h`**
   - Expose `ugfx_ui_set_rotation()` function
   - Document rotation API

#### Touch Input
6. **`main/app_touch.c`**
   - Add coordinate transformation function
   - Apply before gesture processing (~20 lines)

#### Configuration & API
7. **`components/config_store/`**
   - Add rotation config persistence
   - Load/save to NVS

8. **`components/http_api/`**
   - Add `GET/POST /api/rotation` endpoint
   - Expose rotation control via web interface

### µGFX Driver Reference
- **`components/ugfx/drivers/gdisp/framebuffer/gdisp_lld_framebuffer.c`**
  - Rotation already implemented (lines 84-99)
  - Control function (lines 200+)
  - Reference for understanding coordinate transforms

**Estimated Total LOC**: ~150 lines
- Core rotation: ~100 lines
- µGFX integration: ~30 lines
- Touch transform: ~20 lines

