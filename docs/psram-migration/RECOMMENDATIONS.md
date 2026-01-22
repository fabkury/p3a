# PSRAM Migration Recommendations

This document outlines specific recommendations for migrating memory allocations from internal RAM to PSRAM, organized by priority and impact.

## Priority 1: High Impact, Low Risk

These changes have the highest potential to reduce internal RAM pressure with minimal risk of performance issues.

### 1.1 Download Manager Task Stack (80 KB)

**Current**: `main/animation_player_loader.c` line 592 - Uses `xTaskCreate()` which allocates from internal RAM.

**Problem**: This 80KB stack is the largest single allocation from internal RAM (besides DMA buffers).

**Recommendation**: Use `xTaskCreateStatic()` with a pre-allocated PSRAM buffer, following the pattern already used in `makapix_channel_impl.c`.

```c
// Example migration pattern
static StackType_t *s_download_stack = NULL;
static StaticTask_t s_download_task_buffer;

// During init:
s_download_stack = heap_caps_malloc(81920, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (s_download_stack) {
    s_task = xTaskCreateStatic(download_task, "download_mgr", 81920 / sizeof(StackType_t),
                               NULL, 3, s_download_stack, &s_download_task_buffer);
} else {
    // Fallback to internal RAM
    xTaskCreate(download_task, "download_mgr", 81920, NULL, 3, &s_task);
}
```

**Impact**: Frees ~80 KB of internal RAM.

---

### 1.2 Other Large Task Stacks (48 KB combined)

**Tasks to migrate**:
| Task | File | Stack Size |
|------|------|------------|
| `anim_sd_refresh` | `main/animation_player.c:616` | 16 KB |
| `cred_poll` | `components/makapix/makapix_provision_flow.c:61` | 16 KB |
| `mqtt_reconn` | `components/makapix/makapix.c:432` | 16 KB |

**Recommendation**: Apply the same `xTaskCreateStatic()` + PSRAM pattern.

**Impact**: Frees ~48 KB of internal RAM.

---

### 1.3 Explicit PSRAM for Animation Frame Buffers

**Current**: `main/animation_player_loader.c` lines 783, 790 use `malloc()`.

**Analysis**: With `SPIRAM_MALLOC_ALWAYSINTERNAL=16384`, allocations >= 16KB should already go to PSRAM. However, this is implicit behavior that can fail silently.

**Recommendation**: Use explicit PSRAM allocation with fallback:

```c
// Replace:
buf->native_frame_b1 = (uint8_t *)malloc(buf->native_frame_size);

// With:
buf->native_frame_b1 = (uint8_t *)heap_caps_malloc(buf->native_frame_size,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!buf->native_frame_b1) {
    // Fallback to internal RAM for small images
    buf->native_frame_b1 = (uint8_t *)malloc(buf->native_frame_size);
}
```

**Impact**: Ensures large frame buffers (~3 MB) reliably use PSRAM.

---

## Priority 2: Medium Impact, Low Risk

### 2.1 Channel Cache Entry Arrays

**Current**: `components/channel_manager/channel_cache.c` lines 247, 266, 336, 662 use `malloc()`.

**Recommendation**: Use explicit PSRAM allocation for arrays that can exceed 16KB:

```c
// For entries array (up to 64KB):
cache->entries = heap_caps_malloc(entry_count * sizeof(makapix_channel_entry_t),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!cache->entries) {
    cache->entries = malloc(entry_count * sizeof(makapix_channel_entry_t));
}
```

**Impact**: Ensures channel metadata stays in PSRAM for large channels.

---

### 2.2 TLS Certificate Buffers (12 KB)

**Current**: `components/makapix/makapix.c` lines 371-373 allocate 4KB x 3 buffers.

**Recommendation**: Migrate to PSRAM with fallback:

```c
char *ca_cert = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!ca_cert) ca_cert = malloc(4096);
```

**Impact**: Frees ~12 KB of internal RAM during MQTT connection.

---

### 2.3 Decoder Frame Buffers

**Files to update**:
- `components/animation_decoder/png_animation_decoder.c` - lines 171-186
- `components/animated_gif_decoder/gif_animation_decoder.cpp` - line 233

**Recommendation**: Same pattern - explicit PSRAM with fallback.

**Impact**: Variable, depends on image sizes being decoded.

---

## Priority 3: Configuration Changes

### 3.1 Reduce SPIRAM_MALLOC_ALWAYSINTERNAL

**Current**: `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`

**Recommendation**: Consider reducing to 8192 or 4096.

**Trade-off**: Smaller allocations will use PSRAM (slightly slower access), but internal RAM pressure decreases.

**How to change**: `idf.py menuconfig` -> Component config -> ESP PSRAM -> PSRAM config -> Maximum malloc() size in internal memory

---

### 3.2 Enable WiFi/LWIP PSRAM Allocation

**Current**: `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` is **not set**.

**Risk Assessment**: This is a significant change that could affect WiFi/TCP performance.

**Recommendation**: Test enabling this option:
- `idf.py menuconfig` -> Component config -> ESP PSRAM -> PSRAM config -> Try to allocate memories of WiFi and LWIP in SPIRAM

**Expected Impact**: WiFi RX/TX buffers and LWIP buffers move to PSRAM, freeing ~100-200 KB of internal RAM.

**Warning**: This may increase latency for network operations. Requires thorough testing.

---

## Priority 4: Low Impact / Future Consideration

### 4.1 Medium Task Stacks (8 KB each)

Tasks with 8KB stacks could also be migrated:
- `anim_loader`, `app_touch_task`, `ch_switch`, `makapix_prov`, `ota_check`

**Impact**: ~40 KB combined, but more code changes needed.

---

### 4.2 Create Allocation Utility Macros

To standardize the PSRAM-first allocation pattern:

```c
// In a common header
#define PSRAM_MALLOC(size) ({ \
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); \
    if (!p) p = malloc(size); \
    p; \
})

#define PSRAM_CALLOC(count, size) ({ \
    void *p = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); \
    if (!p) p = calloc(count, size); \
    p; \
})
```

---

## Summary of Expected Impact

| Change | Internal RAM Freed | Risk Level |
|--------|-------------------|------------|
| Download manager stack to PSRAM | ~80 KB | Low |
| Other large task stacks | ~48 KB | Low |
| Animation frame buffers explicit | ~0 KB (already PSRAM) | Very Low |
| Channel cache arrays | ~64 KB (peak) | Low |
| TLS certificate buffers | ~12 KB | Low |
| Reduce ALWAYSINTERNAL | Variable | Medium |
| Enable WiFi/LWIP PSRAM | ~100-200 KB | High |

**Total potential savings**: 150-400 KB of internal RAM

---

## Implementation Order

1. **Immediate** (low risk, high impact):
   - Download manager task stack migration
   - Animation frame buffer explicit PSRAM

2. **Short-term** (low risk, medium impact):
   - Other large task stacks
   - Channel cache arrays
   - TLS certificate buffers

3. **Testing phase** (higher risk):
   - Reduce SPIRAM_MALLOC_ALWAYSINTERNAL
   - Enable WiFi/LWIP PSRAM allocation

---

## Testing Checklist

After each change, verify:

- [ ] Device boots successfully
- [ ] WiFi connects and maintains connection
- [ ] MQTT connection is stable
- [ ] Animation playback is smooth
- [ ] No SDIO bus errors during simultaneous SD + WiFi operations
- [ ] Memory reporting shows expected changes in internal/PSRAM usage
