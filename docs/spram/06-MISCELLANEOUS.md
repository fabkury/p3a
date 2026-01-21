# Miscellaneous Components - SPRAM Optimization Analysis

## Component Overview

This document covers remaining components and allocations not covered in other analysis files.

## Components Analyzed

### 1. Config Store (`components/config_store/config_store.c`)

| Allocation | Size Formula | Current Location | Priority |
|------------|--------------|------------------|----------|
| JSON serialization buffers | ~1-5 KB | `malloc()` → Internal | **LOW** |
| Config structures | < 1 KB | `malloc()` → Internal | **VERY LOW** |

**Analysis**:
- Temporary buffers for NVS serialization
- Small allocations, short-lived
- Not performance-critical

**Recommendation**:
```c
// Move JSON buffers to SPIRAM
json_buf = (char *)heap_caps_malloc(buffer_size,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!json_buf) {
    json_buf = malloc(buffer_size);
}
```

**Impact**:
- **Memory freed**: 1-5 KB (temporary)
- **Priority**: Low (small savings)

---

### 2. Slave OTA (`components/slave_ota/slave_ota.c`)

| Allocation | Size Formula | Current Location | Priority |
|------------|--------------|------------------|----------|
| OTA chunk buffer | 1,400 bytes | `malloc()` → Internal | **LOW** |

**Analysis**:
- Small buffer for ESP32-C6 firmware streaming
- Used infrequently (only during co-processor updates)

**Recommendation**:
```c
// Move to SPIRAM
chunk_buffer = (uint8_t *)heap_caps_malloc(OTA_CHUNK_SIZE,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!chunk_buffer) {
    chunk_buffer = malloc(OTA_CHUNK_SIZE);
}
```

**Impact**:
- **Memory freed**: ~1.4 KB
- **Priority**: Low

---

### 3. WiFi Manager (`components/wifi_manager/app_wifi.c`)

| Allocation | Size Formula | Current Location | Priority |
|------------|--------------|------------------|----------|
| HTML page generation | ~2-4 KB | `malloc()` → Internal | **LOW** |
| SSID list buffers | Variable | `malloc()` → Internal | **LOW** |

**Analysis**:
- Temporary buffers for captive portal HTML generation
- Used only during WiFi provisioning
- Not performance-critical

**Recommendation**:
```c
// Move HTML buffers to SPIRAM
html_buffer = (char *)heap_caps_malloc(buffer_size,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!html_buffer) {
    html_buffer = malloc(buffer_size);
}
```

**Impact**:
- **Memory freed**: 2-4 KB (temporary)
- **Priority**: Low

---

### 4. Sync Playlist (`components/sync_playlist/sync_playlist.c`)

| Allocation | Size Formula | Current Location | Priority |
|------------|--------------|------------------|----------|
| `owned_animations` array | count × sizeof(animation_t) | `malloc()` → Internal | **MEDIUM** |

**Analysis**:
- **Line number**: 65 in `sync_playlist.c`
- **Size**: For 100 animations × ~32 bytes = ~3.2 KB
- **Current code**:
  ```c
  S.owned_animations = (animation_t *)malloc(
      (size_t)count * sizeof(animation_t));
  ```

**Recommendation**:
```c
// Move to SPIRAM
S.owned_animations = (animation_t *)heap_caps_malloc(
    (size_t)count * sizeof(animation_t),
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!S.owned_animations) {
    S.owned_animations = (animation_t *)malloc(
        (size_t)count * sizeof(animation_t));
}
```

**Impact**:
- **Memory freed**: 3-10 KB (depending on playlist size)
- **Priority**: Medium

---

### 5. Play Scheduler Cache (`components/play_scheduler/play_scheduler_cache.c`)

| Allocation | Size Formula | Current Location | Priority |
|------------|--------------|------------------|----------|
| Cache data structures | Variable | `malloc()` → Internal | **MEDIUM** |

**Analysis**:
- Similar to channel_cache (covered in 03-CHANNEL-MANAGEMENT.md)
- May have additional temporary buffers

**Recommendation**:
- Follow channel_cache patterns for any large arrays

**Impact**:
- **Memory freed**: Variable, likely small
- **Priority**: Medium (if large allocations found)

---

### 6. µGFX Library (`components/ugfx/`)

| Allocation | Size Formula | Current Location | Priority |
|------------|--------------|------------------|----------|
| Graphics buffers | Variable | `gos_x_heap.c` | **REVIEW** |

**Analysis**:
- Third-party graphics library
- Has own memory allocation abstraction (`gfxAlloc`, `gfxFree`)
- Located in `components/ugfx/src/gos/gos_x_heap.c`

**Recommendation**:
- **INVESTIGATE**: Check if µGFX can be configured to use SPIRAM
- **Complexity**: High (third-party library integration)
- **Priority**: Low (unless significant memory usage found)

**Note**: This requires deeper investigation into µGFX memory configuration.

---

### 7. P3A Core Components

#### p3a_logo.c
- Embedded logo data (read-only, in flash)
- **No changes needed**

#### p3a_boot_logo.c
- Boot logo rendering
- Temporary buffers (if any)
- **Priority**: Very low

#### p3a_render.c
- State-aware rendering
- May share buffers with display_renderer
- **Priority**: Low (likely using existing buffers)

---

## Stack vs Heap Allocations

### Stack-Based Buffers

Some functions use large stack buffers that could benefit from heap allocation:

**Example**: WebSocket frame buffer in `http_api_pico8.c`
```c
// Stack allocation
uint8_t ws_frame[WS_MAX_FRAME_SIZE];  // 8246 bytes on stack
```

**Recommendation**:
```c
// Move to heap + SPIRAM
uint8_t *ws_frame = (uint8_t *)heap_caps_malloc(
    WS_MAX_FRAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!ws_frame) {
    ws_frame = (uint8_t *)malloc(WS_MAX_FRAME_SIZE);
}
// Use buffer
free(ws_frame);
```

**Impact**:
- **Stack freed**: 8 KB per WebSocket connection
- **Priority**: Low (unless stack overflow occurs)

---

## Summary of Miscellaneous Components

| Component | Memory | Priority | Impact |
|-----------|--------|----------|--------|
| Config Store JSON | 1-5 KB | Low | Small |
| Slave OTA chunk | 1.4 KB | Low | Small |
| WiFi Manager HTML | 2-4 KB | Low | Small |
| Sync Playlist array | 3-10 KB | Medium | Small-Medium |
| Play Scheduler cache | Variable | Medium | TBD |
| µGFX library | Unknown | Low | Needs investigation |
| WebSocket stack buffer | 8 KB | Low | Medium (if moved) |

### Total Potential Savings: ~15-30 KB

### Implementation Priority

1. **Medium**: Sync playlist animations array (3-10 KB)
2. **Low**: Config Store JSON buffers (1-5 KB)
3. **Low**: WiFi Manager HTML buffers (2-4 KB)
4. **Low**: Slave OTA chunk buffer (1.4 KB)
5. **Very Low**: WebSocket stack-to-heap conversion (8 KB, only if needed)

### Risk Assessment

**Very Low Risk**:
- All components are non-critical
- Small allocations with minimal performance impact
- Easy to fallback to internal RAM

### Code Changes Required

- **Files to modify**: 4-6 files
- **Lines to change**: ~10-20 lines total
- **Complexity**: Low
- **Testing effort**: Low

---

## Special Cases

### Read-Only Data (RODATA)

Some large data structures are read-only and should be in flash, not RAM:

**Example**: Logo images, font data, lookup tables

These typically use:
```c
static const uint8_t logo_data[] = { ... };
```

**Recommendation**: Ensure these use `const` to keep them in flash (RODATA)

**Note**: With `CONFIG_SPIRAM_RODATA=y`, read-only data may be in SPIRAM already.

---

### DMA Buffers

Some buffers must be DMA-capable:
- **USB buffers**: Already handled by TinyUSB
- **SDMMC buffers**: Already handled by SDMMC driver
- **LCD buffers**: Already handled by LCD driver

**Recommendation**: Keep DMA buffers as-is (drivers handle allocation)

---

### Static vs Dynamic Allocation

Some components may benefit from static allocation:

**Current pattern**:
```c
static my_struct_t *s_instance = NULL;

void init() {
    s_instance = malloc(sizeof(my_struct_t));
}
```

**Alternative pattern**:
```c
static my_struct_t s_instance;  // Static allocation, zero-initialized

void init() {
    memset(&s_instance, 0, sizeof(s_instance));
}
```

**Benefits**:
- No malloc/free needed
- No fragmentation
- Slightly faster

**Consideration**: Static allocation uses .bss segment (internal RAM by default)

---

## Recommendations for Future Code

### Best Practices for New Code

1. **Large buffers** (> 10 KB): Always use SPIRAM first
   ```c
   ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!ptr) ptr = malloc(size);  // Fallback
   ```

2. **Small, frequently accessed**: Keep in internal RAM
   ```c
   ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL);
   ```

3. **DMA buffers**: Use explicit DMA capability
   ```c
   ptr = heap_caps_malloc(size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
   ```

4. **Task stacks** (non-critical): Use SPIRAM
   ```c
   stack = heap_caps_malloc(stack_size * sizeof(StackType_t),
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   xTaskCreateStatic(..., stack, ...);
   ```

5. **Read-only data**: Use `const` to keep in flash
   ```c
   static const uint8_t data[] = { ... };  // In flash, not RAM
   ```

### Code Review Checklist

When reviewing new allocations, check:
- [ ] Size > 10 KB → Should use SPIRAM
- [ ] Network/file buffer → Should use SPIRAM
- [ ] Image/frame buffer → Should use SPIRAM
- [ ] Task stack (non-critical) → Should use SPIRAM
- [ ] Lookup table (< 10 KB, frequently accessed) → Keep internal
- [ ] DMA buffer → Use MALLOC_CAP_DMA
- [ ] Read-only data → Use `const` for flash storage

---

**Recommendation Status**: ✅ **APPROVED FOR IMPLEMENTATION (Low Priority)**  
**Expected Impact**: **LOW** (15-30 KB)  
**Risk Level**: **VERY LOW**  
**Effort**: **LOW**

**Note**: These are nice-to-have optimizations after high-priority items are complete.
