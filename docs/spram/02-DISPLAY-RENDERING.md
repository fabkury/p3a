# Display Rendering - SPRAM Optimization Analysis

## Component Overview

The display renderer manages frame buffers, upscaling, and presentation to the LCD panel. This component includes both performance-critical and non-critical memory allocations.

**Files:**
- `main/display_renderer.c`
- `main/display_renderer_priv.h`
- `main/animation_player_loader.c`
- `components/pico8/pico8_render.c`

## Current Allocations

### 1. Display Frame Buffers (`display_renderer.c`)

| Allocation | Size Formula | Current Location | Priority |
|------------|--------------|------------------|----------|
| `g_display_buffers` | 720×720×3 × 2-3 buffers | BSP/Driver managed | **KEEP INTERNAL** |
| Buffer pointers array | ~12-24 bytes | Stack/Global | N/A |

**Analysis:**
- **Frame buffers**: Allocated by BSP/LCD driver, likely already in PSRAM
- **Location**: Managed by `waveshare__esp32_p4_wifi6_touch_lcd_4b` BSP component
- **Size**: ~1.5 MB per buffer × 2-3 buffers = 3-4.5 MB total
- **Current code**: `display_renderer_init()` receives pre-allocated buffers

**Recommendation:**
- **NO CHANGE NEEDED** - BSP driver should already be using PSRAM for frame buffers
- **Verification**: Check BSP configuration to confirm PSRAM usage
- **Note**: These buffers MUST be in DMA-capable memory (PSRAM is DMA-capable on ESP32-P4)

---

### 2. Upscale Lookup Tables (`animation_player_loader.c`)

| Allocation | Size Formula | Current Location | Priority |
|------------|--------------|------------------|----------|
| `upscale_lookup_x` | max_dim × sizeof(uint16_t) | `MALLOC_CAP_INTERNAL` | **KEEP INTERNAL** ✅ |
| `upscale_lookup_y` | max_dim × sizeof(uint16_t) | `MALLOC_CAP_INTERNAL` | **KEEP INTERNAL** ✅ |

**Analysis:**
- **Size**: 720 × 2 bytes × 2 arrays = 2,880 bytes (2.8 KB total)
- **Line numbers**: Lines 254-255 in `animation_player_loader.c`
- **Current code**:
  ```c
  buf->upscale_lookup_x = (uint16_t *)heap_caps_malloc(
      (size_t)max_len * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
  buf->upscale_lookup_y = (uint16_t *)heap_caps_malloc(
      (size_t)max_len * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
  ```

**Recommendation:**
- **KEEP IN INTERNAL RAM** - These are accessed in tight loops during upscaling
- **Justification**: 
  - Small size (< 3 KB)
  - Performance-critical (accessed every pixel during upscaling)
  - Fast access needed for real-time rendering
- **NO CHANGE NEEDED** ✅

---

### 3. PICO-8 Frame Buffers (`pico8_render.c`)

| Allocation | Size Formula | Current Location | Priority |
|------------|--------------|------------------|----------|
| `frame_buffers[0]` | 128×128×4 bytes | `MALLOC_CAP_SPIRAM` | **Already Optimized** ✅ |
| `frame_buffers[1]` | 128×128×4 bytes | `MALLOC_CAP_SPIRAM` | **Already Optimized** ✅ |
| `lookup_x` | 720 × sizeof(uint16_t) | `MALLOC_CAP_INTERNAL` | **KEEP INTERNAL** ✅ |
| `lookup_y` | 720 × sizeof(uint16_t) | `MALLOC_CAP_INTERNAL` | **KEEP INTERNAL** ✅ |

**Analysis:**
- **Frame buffers**: 128×128×4 × 2 = 131,072 bytes (128 KB total)
- **Lookup tables**: 720 × 2 × 2 = 2,880 bytes (2.8 KB total)
- **Line numbers**: 103 (frame buffers), 134-135 (lookups) in `pico8_render.c`
- **Current code**:
  ```c
  s_pico8.frame_buffers[i] = (uint8_t *)heap_caps_malloc(
      frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  
  s_pico8.lookup_x = (uint16_t *)heap_caps_malloc(
      (size_t)scaled_w * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
  s_pico8.lookup_y = (uint16_t *)heap_caps_malloc(
      (size_t)scaled_h * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
  ```

**Recommendation:**
- **NO CHANGE NEEDED** ✅
- **Justification**:
  - Frame buffers already in SPIRAM (correct)
  - Lookup tables in internal RAM (correct for performance)
- **Status**: **Already optimized**

---

### 4. Native Animation Frame Buffers (`animation_player_loader.c`)

| Allocation | Size Formula | Current Location | Priority |
|------------|--------------|------------------|----------|
| `native_frame_b1` | native_w × native_h × 4 | `malloc()` → Internal | **MEDIUM** |
| `native_frame_b2` | native_w × native_h × 4 | `malloc()` → Internal | **MEDIUM** |

**Analysis:**
- **Size**: Variable, up to 720×720×4 = 2,073,600 bytes (2 MB) per buffer
- **Total**: Up to 4 MB for both buffers
- **Usage**: Double-buffering for native resolution frames before upscaling
- **Note**: These are allocated in `animation_player_loader.c` but actual allocation code needs verification

**Recommendation:**
```c
// Move to SPIRAM with fallback
buf->native_frame_b1 = (uint8_t *)heap_caps_malloc(
    buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!buf->native_frame_b1) {
    buf->native_frame_b1 = (uint8_t *)malloc(buffer_size);
}
```

**Impact:**
- **Memory freed**: Up to 4 MB
- **Performance**: Minimal impact (frames are rendered sequentially, not in tight loops)

**Note**: Need to locate exact allocation code to confirm this recommendation.

---

### 5. Upscale Worker Task Stacks (`display_renderer.c`)

| Allocation | Size Formula | Current Location | Priority |
|------------|--------------|------------------|----------|
| `upscale_top` task stack | 2048 bytes | Internal (xTaskCreate) | **MEDIUM** |
| `upscale_bottom` task stack | 2048 bytes | Internal (xTaskCreate) | **MEDIUM** |
| `display_render_task` stack | 4096 bytes | Internal (xTaskCreate) | **MEDIUM** |

**Analysis:**
- **Total**: 8,192 bytes (8 KB)
- **Line numbers**: 136-142, 148-154, 334-340 in `display_renderer.c`
- **Current code**:
  ```c
  xTaskCreatePinnedToCore(display_upscale_worker_top_task,
                          "upscale_top", 2048, NULL, ...);
  xTaskCreatePinnedToCore(display_upscale_worker_bottom_task,
                          "upscale_bottom", 2048, NULL, ...);
  xTaskCreate(display_render_task, "display_render", 4096, NULL, ...);
  ```

**Recommendation:**
```c
// Pre-allocate stacks in SPIRAM using xTaskCreateStatic
static StackType_t upscale_top_stack[2048];
static StackType_t upscale_bottom_stack[2048];
static StackType_t render_task_stack[4096];

// Allocate these static arrays with __attribute__((section(".spiram.data")))
// OR use heap_caps_malloc + xTaskCreateStatic

StackType_t *top_stack = (StackType_t *)heap_caps_malloc(
    2048 * sizeof(StackType_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

xTaskCreateStatic(display_upscale_worker_top_task, "upscale_top",
                  2048, NULL, priority, top_stack, &top_task_buffer);
```

**Impact:**
- **Memory freed**: 8 KB
- **Performance**: Minimal impact (task stacks are accessed by FreeRTOS scheduler, not performance-critical)

**Complexity**: Medium (requires switching from dynamic to static task creation)

---

## Summary

### Current Status

| Component | Status | Memory | Recommendation |
|-----------|--------|--------|----------------|
| Display frame buffers | ✅ Likely PSRAM | 3-4.5 MB | Verify only |
| Upscale lookup tables | ✅ Internal RAM | 2.8 KB | Keep as-is |
| PICO-8 frame buffers | ✅ SPIRAM | 128 KB | Already optimized |
| PICO-8 lookups | ✅ Internal RAM | 2.8 KB | Keep as-is |
| Native frame buffers | ⚠️ Internal RAM | Up to 4 MB | **Move to SPIRAM** |
| Upscale task stacks | ⚠️ Internal RAM | 8 KB | **Move to SPIRAM** |

### Total Potential Savings

- **Native frame buffers**: Up to 4 MB (if allocated)
- **Task stacks**: 8 KB
- **Total**: ~4 MB (high priority items only)

### Implementation Priority

1. **High**: Native animation frame buffers (4 MB potential savings)
2. **Medium**: Upscale task stacks (8 KB savings, moderate complexity)
3. **Low**: Verify display frame buffers already in PSRAM

### Risk Assessment

**Medium Risk (Native Frame Buffers):**
- Need to verify allocation location first
- Test upscaling performance thoroughly
- Ensure no regressions in frame rate

**Low Risk (Task Stacks):**
- Task stacks can safely be in SPIRAM
- FreeRTOS supports SPIRAM task stacks
- No performance impact expected

---

**Recommendation Status**: ⚠️ **NEEDS VERIFICATION**  
**Expected Impact**: **MEDIUM** (up to 4 MB if native buffers found)  
**Risk Level**: **MEDIUM**  
**Effort**: **MEDIUM**

**Next Steps:**
1. Locate native frame buffer allocation code
2. Verify display frame buffer location (BSP component)
3. Test upscaling performance with SPIRAM buffers
4. Implement task stack changes if performance acceptable
