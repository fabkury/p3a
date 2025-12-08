# Transparency and GIF Disposal Mode Implementation Plan

## Executive Summary

This document provides a detailed implementation plan to properly handle:
1. **GIF disposal modes** (restore to background vs. keep previous frame)
2. **Transparency optimization** across all supported formats (GIF, WebP, PNG)
3. **Performance optimization** through format-specific code paths

## Current State Analysis

### What Works
- Basic transparency handling in GIFs (transparent pixels copy from previous frame)
- PNG transparency detection and RGBA conversion
- WebP transparency detection via alpha channel
- RGBA to RGB565/RGB888 conversion in display renderer

### What's Missing/Broken

#### 1. GIF Disposal Modes (Critical Issue)
**Current behavior:** The GIF decoder partially implements disposal mode 2 (restore to background) in the internal AnimatedGIF library (`gif.inl`), but this is **only used when a framebuffer is allocated**. The wrapper code in `gif_animation_decoder.cpp` doesn't properly respect disposal methods:

- **Line 256-261 in `gif_animation_decoder.cpp`:** Before decoding each frame, the code copies the previous frame and then clears the RGBA buffer to black (0,0,0,0). This is incorrect!
- **Line 67-76 in callback:** Transparent pixels are handled by copying from `previous_frame`, but this doesn't account for different disposal methods.

**GIF Disposal Methods (from GIF89a spec):**
- **0 (No disposal specified):** Leave frame in place for next frame
- **1 (Do not dispose):** Leave frame in place (same as 0)
- **2 (Restore to background color):** Clear the frame area to the background color
- **3 (Restore to previous):** Restore to the state before the frame was drawn
- **4-7:** (Reserved, treat as 0)

**Problem:** When disposal method is 2, we should fill transparent areas with the background color, not the previous frame. When disposal method is 1, we should keep the previous frame. Currently, the code always keeps the previous frame.

#### 2. Transparency Optimization
All formats currently use a single code path that always processes the alpha channel, even when images are fully opaque:

**Current flow:**
1. Decode to RGBA (4 bytes per pixel)
2. Upscale with rotation in display_renderer
3. Convert RGBA→RGB565 (or RGB888), discarding alpha

**Performance impact:**
- 25% more memory bandwidth (4 bytes vs 3 bytes per pixel)
- Alpha channel is loaded from memory but never used for opaque images
- Cache pressure increased unnecessarily

#### 3. Format-Specific Issues

**PNG (still images):**
- Always decodes to RGBA even when the image is opaque
- Alpha channel is discarded during rendering
- No optimized path for opaque PNGs

**WebP (animated and still):**
- libwebp always outputs RGBA for animations
- Still images always decode to RGBA regardless of alpha presence
- `has_alpha` detection exists but isn't used for optimization

**GIF:**
- Transparent pixels in GIF are handled correctly in isolation
- Disposal methods are not properly implemented in the wrapper
- No separate code path for opaque GIFs

## Proposed Solution Architecture

### Design Principles
1. **Minimize computational cost** - Use RGB (3-byte) path for opaque images
2. **Correct behavior** - Properly implement GIF disposal modes
3. **Backward compatibility** - Don't break existing functionality
4. **Minimal code changes** - Surgical modifications only

### Implementation Strategy

```
┌─────────────────────────────────────────────────────────────┐
│                    Decoder Layer                            │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐     │
│  │ GIF Decoder  │  │ WebP Decoder │  │ PNG Decoder  │     │
│  │              │  │              │  │              │     │
│  │ - Detect     │  │ - Detect     │  │ - Detect     │     │
│  │   opacity    │  │   opacity    │  │   opacity    │     │
│  │ - Handle     │  │ - Output RGB │  │ - Output RGB │     │
│  │   disposal   │  │   or RGBA    │  │   or RGBA    │     │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘     │
└─────────┼──────────────────┼──────────────────┼────────────┘
          │                  │                  │
          └──────────────────┴──────────────────┘
                             │
          ┌──────────────────▼───────────────────────┐
          │     animation_decoder_info_t             │
          │  + has_transparency: bool                │
          │  + pixel_format: RGB or RGBA             │
          └──────────────────┬───────────────────────┘
                             │
          ┌──────────────────▼───────────────────────┐
          │         Animation Player                  │
          │  Allocates RGB or RGBA buffers based     │
          │  on decoder capabilities                  │
          └──────────────────┬───────────────────────┘
                             │
          ┌──────────────────▼───────────────────────┐
          │       Display Renderer                    │
          │  - upscale_rgb() for opaque              │
          │  - upscale_rgba() for transparent        │
          └───────────────────────────────────────────┘
```

## Detailed Implementation Plan

### Phase 1: Extend Decoder Interface

**File:** `components/animation_decoder/include/animation_decoder.h`

**Changes:**
1. Add pixel format enum:
```c
typedef enum {
    ANIMATION_PIXEL_FORMAT_RGB,   // 3 bytes per pixel, no transparency
    ANIMATION_PIXEL_FORMAT_RGBA   // 4 bytes per pixel, with alpha
} animation_pixel_format_t;
```

2. Extend `animation_decoder_info_t`:
```c
typedef struct {
    uint32_t canvas_width;
    uint32_t canvas_height;
    size_t frame_count;
    bool has_transparency;
    animation_pixel_format_t pixel_format;  // NEW: preferred output format
} animation_decoder_info_t;
```

3. Add new decode function for RGB output:
```c
/**
 * @brief Decode the next frame to RGB buffer (no alpha)
 * 
 * Only available when decoder reports pixel_format == ANIMATION_PIXEL_FORMAT_RGB
 * Buffer must be at least canvas_width * canvas_height * 3 bytes
 */
esp_err_t animation_decoder_decode_next_rgb(animation_decoder_t *decoder, uint8_t *rgb_buffer);
```

### Phase 2: Fix GIF Disposal Modes

**File:** `components/animated_gif_decoder/gif_animation_decoder.cpp`

**Root cause:** The disposal method information is available in the GIFDRAW structure but not being used correctly in the wrapper.

**Solution:** 
1. Store the disposal method in `gif_decoder_impl`
2. In the draw callback, apply disposal correctly:
   - Disposal 0/1: Keep previous frame for transparent pixels
   - Disposal 2: Use background color for transparent pixels
   - Disposal 3: Restore to state before current frame (requires second previous frame buffer)

**Changes:**

```cpp
struct gif_decoder_impl {
    AnimatedGIF *gif;
    uint8_t *rgba_buffer;
    uint32_t canvas_width;
    uint32_t canvas_height;
    size_t frame_count;
    size_t current_frame;
    bool initialized;
    const uint8_t *file_data;
    size_t file_size;
    uint8_t *previous_frame;           // Frame before current
    uint8_t *restore_frame;            // NEW: Frame before previous (for disposal 3)
    uint8_t current_disposal_method;   // NEW: Disposal method for current frame
    uint8_t background_color_index;    // NEW: Background color from GIF
    uint8_t background_rgba[4];        // NEW: Converted background color
    uint32_t current_frame_delay_ms;
    bool has_transparency;             // NEW: Track if any frame has transparency
};
```

**Modified draw callback:**
```cpp
static void gif_draw_callback(GIFDRAW *pDraw)
{
    struct gif_decoder_impl *impl = (struct gif_decoder_impl *)pDraw->pUser;
    if (!impl || !impl->rgba_buffer) {
        return;
    }
    
    // Store disposal method for next frame
    impl->current_disposal_method = pDraw->ucDisposalMethod;
    
    const int canvas_w = (int)impl->canvas_width;
    const int y = pDraw->y;
    const int frame_x = pDraw->iX;
    const int frame_y = pDraw->iY;
    const int frame_w = pDraw->iWidth;
    
    uint8_t *dst_row = impl->rgba_buffer + (size_t)(frame_y + y) * canvas_w * 4;
    uint8_t *palette24 = pDraw->pPalette24;
    uint8_t transparent = pDraw->ucTransparent;
    bool has_transparency = pDraw->ucHasTransparency;
    
    for (int x = 0; x < frame_w; x++) {
        uint8_t pixel_index = pDraw->pPixels[x];
        uint8_t *dst_pixel = dst_row + (size_t)(frame_x + x) * 4;
        
        if (has_transparency && pixel_index == transparent) {
            // Handle transparency based on PREVIOUS frame's disposal method
            // (disposal method affects what to do AFTER displaying the frame)
            if (impl->previous_disposal_method == 2) {
                // Previous frame said "restore to background"
                memcpy(dst_pixel, impl->background_rgba, 4);
            } else if (impl->previous_disposal_method == 3) {
                // Previous frame said "restore to previous"
                if (impl->restore_frame) {
                    memcpy(dst_pixel, impl->restore_frame + (size_t)(frame_y + y) * canvas_w * 4 + (size_t)(frame_x + x) * 4, 4);
                } else {
                    memcpy(dst_pixel, impl->background_rgba, 4);
                }
            } else {
                // Disposal 0/1: keep previous frame
                if (impl->previous_frame) {
                    memcpy(dst_pixel, impl->previous_frame + (size_t)(frame_y + y) * canvas_w * 4 + (size_t)(frame_x + x) * 4, 4);
                } else {
                    memcpy(dst_pixel, impl->background_rgba, 4);
                }
            }
        } else {
            // Opaque pixel
            uint8_t *palette_entry = palette24 + pixel_index * 3;
            dst_pixel[0] = palette_entry[0]; // R
            dst_pixel[1] = palette_entry[1]; // G
            dst_pixel[2] = palette_entry[2]; // B
            dst_pixel[3] = 255; // A
        }
        
        if ((x % 32) == 31) {
            taskYIELD();
        }
    }
}
```

**Modified decode_next:**
```cpp
esp_err_t gif_decoder_decode_next(animation_decoder_t *decoder, uint8_t *rgba_buffer)
{
    // ... validation code ...
    
    // Rotate frame buffers for disposal method 3 support
    size_t rgba_size = (size_t)impl->canvas_width * impl->canvas_height * 4;
    if (impl->current_disposal_method == 3 && impl->restore_frame) {
        // Swap: restore_frame ← previous_frame ← rgba_buffer
        uint8_t *temp = impl->restore_frame;
        impl->restore_frame = impl->previous_frame;
        impl->previous_frame = impl->rgba_buffer;
        impl->rgba_buffer = temp;
    } else if (impl->current_disposal_method == 2) {
        // For disposal 2, we start with background color
        // Fill with background color
        for (size_t i = 0; i < rgba_size; i += 4) {
            memcpy(impl->rgba_buffer + i, impl->background_rgba, 4);
        }
    } else {
        // Disposal 0/1: copy previous frame
        if (impl->previous_frame) {
            memcpy(impl->rgba_buffer, impl->previous_frame, rgba_size);
        }
    }
    
    // Decode frame (callback will update pixels)
    int delay_ms = 0;
    int result = impl->gif->playFrame(false, &delay_ms, impl);
    
    // ... rest of function ...
}
```

### Phase 3: Implement RGB Output Path for Opaque Images

**Files to modify:**
- `components/animation_decoder/png_animation_decoder.c`
- `components/animation_decoder/webp_animation_decoder.c`
- `components/animated_gif_decoder/gif_animation_decoder.cpp`

**Strategy:** Add a `decode_next_rgb()` function to each decoder that outputs RGB (3 bytes/pixel) when the image is opaque.

**PNG Decoder Changes:**
```c
esp_err_t png_decoder_init(animation_decoder_t **decoder, const uint8_t *data, size_t size)
{
    // ... existing code ...
    
    // After detecting transparency, decide pixel format
    png_data->has_transparency = (color_type & PNG_COLOR_MASK_ALPHA) != 0 || 
                                  png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS);
    
    // NEW: Decode to RGB if no transparency, RGBA if transparent
    if (!png_data->has_transparency) {
        // Remove alpha channel transformation
        // Decode directly to RGB
        png_data->pixel_format = ANIMATION_PIXEL_FORMAT_RGB;
        png_data->rgba_buffer_size = (size_t)width * height * 3;
    } else {
        // Keep RGBA
        png_data->pixel_format = ANIMATION_PIXEL_FORMAT_RGBA;
        png_data->rgba_buffer_size = (size_t)width * height * 4;
    }
    
    // ... rest of decode logic ...
}
```

**WebP Decoder Changes:**
```c
// For still images
if (!webp_data->is_animation) {
    if (features.has_alpha) {
        // Decode to RGBA
        webp_data->pixel_format = ANIMATION_PIXEL_FORMAT_RGBA;
        const size_t frame_size = (size_t)features.width * features.height * 4;
        webp_data->still_rgba = (uint8_t *)malloc(frame_size);
        WebPDecodeRGBAInto(data, size, webp_data->still_rgba, frame_size, stride);
    } else {
        // NEW: Decode to RGB for opaque images
        webp_data->pixel_format = ANIMATION_PIXEL_FORMAT_RGB;
        const size_t frame_size = (size_t)features.width * features.height * 3;
        webp_data->still_rgb = (uint8_t *)malloc(frame_size);
        WebPDecodeRGBInto(data, size, webp_data->still_rgb, frame_size, stride_rgb);
    }
}
```

**GIF Decoder Changes:**
```c
// Detect if ANY frame in the GIF has transparency
// This requires parsing all frame headers during init
// Store in gif_decoder_impl->has_transparency

// If no transparency anywhere, can output RGB
if (!impl->has_transparency) {
    impl->pixel_format = ANIMATION_PIXEL_FORMAT_RGB;
    // Can skip previous_frame buffer allocation
} else {
    impl->pixel_format = ANIMATION_PIXEL_FORMAT_RGBA;
    // Need previous_frame buffer for disposal
}
```

### Phase 4: Update Display Renderer

**File:** `main/display_renderer.c`

**Changes:** Add optimized RGB upscale path (no alpha channel processing)

```c
// New function for RGB input (3 bytes per pixel)
void display_renderer_parallel_upscale_rgb(
    const uint8_t *src_rgb,    // RGB input (3 bytes/pixel)
    int src_w, int src_h,
    uint8_t *dst_buffer,
    const uint16_t *lookup_x,
    const uint16_t *lookup_y,
    display_rotation_t rotation)
{
    // Similar to existing function but:
    // 1. Read RGB (3 bytes) instead of RGBA (4 bytes)
    // 2. Skip alpha channel processing
    // 3. ~25% less memory bandwidth
    
    const uint8_t *src_rgb8 = src_rgb;  // 3 bytes per pixel
    
    // Per pixel:
    const uint8_t r = src_rgb8[pixel_offset * 3 + 0];
    const uint8_t g = src_rgb8[pixel_offset * 3 + 1];
    const uint8_t b = src_rgb8[pixel_offset * 3 + 2];
    // No alpha channel to read!
}
```

### Phase 5: Update Animation Player

**File:** `main/animation_player_loader.c` and `animation_player_render.c`

**Changes:**
1. Query decoder for pixel format during initialization
2. Allocate appropriately sized buffers (RGB = 3 bytes, RGBA = 4 bytes)
3. Call correct decode function based on format
4. Use correct upscale function

```c
// In animation buffer allocation
animation_decoder_info_t info;
animation_decoder_get_info(decoder, &info);

if (info.pixel_format == ANIMATION_PIXEL_FORMAT_RGB) {
    // Allocate RGB buffers (3 bytes per pixel)
    buf->native_frame_b1 = malloc(info.canvas_width * info.canvas_height * 3);
    buf->bytes_per_pixel = 3;
} else {
    // Allocate RGBA buffers (4 bytes per pixel)
    buf->native_frame_b1 = malloc(info.canvas_width * info.canvas_height * 4);
    buf->bytes_per_pixel = 4;
}

// In render function
if (buf->bytes_per_pixel == 3) {
    // Use RGB decode
    animation_decoder_decode_next_rgb(buf->decoder, decode_buffer);
    display_renderer_parallel_upscale_rgb(decode_buffer, ...);
} else {
    // Use RGBA decode
    animation_decoder_decode_next(buf->decoder, decode_buffer);
    display_renderer_parallel_upscale(decode_buffer, ...);  // existing RGBA path
}
```

## Testing Strategy

### Test Cases

#### GIF Disposal Modes
1. **Test GIF with disposal method 0/1** (keep previous)
   - Create GIF with multiple frames where each frame only draws part of the image
   - Verify frames accumulate correctly
   
2. **Test GIF with disposal method 2** (restore to background)
   - Create GIF where frames should not accumulate
   - Verify transparent areas show background color, not previous frame
   
3. **Test GIF with disposal method 3** (restore to previous)
   - Create GIF with alternating full and partial frames
   - Verify correct restoration behavior

4. **Test GIF with mixed disposal methods**
   - Create GIF using different disposal methods on different frames
   - Verify correct behavior for each frame

#### Transparency Handling
1. **Opaque PNG** - Verify RGB path is used (3 bytes/pixel)
2. **Transparent PNG** - Verify RGBA path is used (4 bytes/pixel)
3. **Opaque WebP (still)** - Verify RGB path is used
4. **Transparent WebP (still)** - Verify RGBA path is used
5. **Animated WebP with alpha** - Verify RGBA path is used
6. **Opaque GIF** - Verify RGB path can be used
7. **Transparent GIF** - Verify RGBA path and disposal modes work

#### Performance Tests
1. **Memory bandwidth comparison**
   - Measure decode → upscale time for RGB vs RGBA path
   - Expect ~15-20% improvement for RGB path due to reduced memory traffic

2. **Frame rate consistency**
   - Verify target frame rates are maintained with new code
   - Check for any regressions

### Test Implementation

**Location:** Create test files in existing structure or add to component tests

```c
// Example test structure
void test_gif_disposal_method_2() {
    // Load test GIF with disposal method 2
    const uint8_t *gif_data = ...;
    
    animation_decoder_t *decoder;
    gif_decoder_init(&decoder, gif_data, gif_size);
    
    uint8_t *frame1 = malloc(...);
    uint8_t *frame2 = malloc(...);
    
    // Decode frame 1
    animation_decoder_decode_next(decoder, frame1);
    
    // Decode frame 2 (should clear to background, not keep frame 1)
    animation_decoder_decode_next(decoder, frame2);
    
    // Verify transparent pixels in frame2 show background color
    // NOT pixels from frame1
    assert_background_color(frame2, transparent_region);
    
    // Cleanup
    gif_decoder_unload(&decoder);
    free(frame1);
    free(frame2);
}
```

## Performance Optimization Summary

### Expected Improvements

| Scenario | Current | Optimized | Improvement |
|----------|---------|-----------|-------------|
| Opaque PNG decode & upscale | RGBA (4 bytes) | RGB (3 bytes) | ~25% less memory bandwidth |
| Opaque WebP decode & upscale | RGBA (4 bytes) | RGB (3 bytes) | ~25% less memory bandwidth |
| Opaque GIF decode & upscale | RGBA (4 bytes) | RGB (3 bytes) | ~25% less memory bandwidth |
| Transparent GIF | Partial disposal support | Full disposal support | Correct rendering |

### Memory Savings

For a typical 128×128 pixel animation:
- **Current:** 128 × 128 × 4 × 2 buffers = 131,072 bytes
- **Optimized (opaque):** 128 × 128 × 3 × 2 buffers = 98,304 bytes
- **Savings:** 32,768 bytes (25%) per animation

With double-buffering for 720×720 display:
- **Current:** 720 × 720 × 4 × 2 = 4,147,200 bytes
- **Optimized (opaque):** 720 × 720 × 3 × 2 = 3,110,400 bytes
- **Savings:** 1,036,800 bytes (25%)

## Implementation Phases and Effort Estimates

### Phase 1: Interface Extensions (2 hours)
- Extend animation_decoder.h with pixel format enum
- Add decode_next_rgb() function declaration
- Update documentation

### Phase 2: Fix GIF Disposal Modes (4 hours)
- Implement disposal method tracking in gif_decoder_impl
- Update draw callback to handle all disposal modes
- Add restore_frame buffer for disposal mode 3
- Test with various GIF files

### Phase 3: RGB Output Path (6 hours)
- Implement RGB decode for PNG (2 hours)
- Implement RGB decode for WebP (2 hours)
- Implement RGB decode for GIF (2 hours)
- Test each decoder

### Phase 4: Display Renderer Updates (3 hours)
- Implement display_renderer_parallel_upscale_rgb()
- Optimize for all rotation modes
- Test rendering pipeline

### Phase 5: Animation Player Integration (2 hours)
- Update buffer allocation logic
- Update render function routing
- Integration testing

### Phase 6: Testing & Validation (4 hours)
- Create test GIF files with various disposal modes
- Create test cases for RGB/RGBA paths
- Performance benchmarking
- Fix any bugs discovered

### Total Estimated Effort: 21 hours

## Rollout Strategy

### Development Sequence
1. **Phase 1-2 First:** Fix disposal modes as a standalone improvement
   - Can be merged independently
   - Fixes incorrect GIF rendering
   
2. **Phase 3-5 Second:** Add RGB optimization as enhancement
   - Builds on fixed disposal implementation
   - Provides performance benefit

3. **Phase 6 Throughout:** Continuous testing

### Backward Compatibility
- All existing RGBA code paths remain functional
- New RGB paths are additive (decoder reports capability)
- Fallback to RGBA if RGB decode not supported

### Configuration Options
Consider adding config options:
```c
// In Kconfig
config ANIMATION_ENABLE_RGB_OPTIMIZATION
    bool "Enable RGB output optimization for opaque images"
    default y
    help
        When enabled, decoders will output RGB (3 bytes/pixel) for opaque
        images instead of RGBA (4 bytes/pixel). This reduces memory bandwidth
        by 25% for opaque animations.
```

## Future Enhancements

### Beyond This Plan
1. **SIMD optimizations** for upscaling (ARM NEON)
2. **Indexed color path** for palette-based GIFs (1 byte/pixel)
3. **Hardware acceleration** if available on ESP32-P4
4. **Adaptive buffer sizing** based on available RAM
5. **Progressive rendering** for large images

### Monitoring
Add metrics to track:
- Percentage of animations using RGB vs RGBA path
- Average decode + upscale time per format
- Memory utilization per animation
- GIF disposal method distribution in the wild

## Conclusion

This implementation plan provides:
1. ✅ **Correct GIF disposal mode handling** (fixes rendering bugs)
2. ✅ **Optimized transparency handling** (25% memory bandwidth reduction for opaque images)
3. ✅ **Minimal code changes** (surgical modifications to existing code)
4. ✅ **Backward compatibility** (all existing functionality preserved)
5. ✅ **Clear testing strategy** (comprehensive validation)

The changes are structured in phases that can be implemented and tested incrementally, reducing risk and allowing for early benefits (disposal mode fixes) while working toward the full optimization (RGB paths).
