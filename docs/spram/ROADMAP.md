# SPRAM Optimization - Implementation Roadmap

## Executive Summary

This document provides a **prioritized roadmap** for implementing SPRAM (SPIRAM) optimizations across the p3a codebase. Based on comprehensive analysis of memory allocations, we've identified opportunities to free **up to 4.5 MB** of internal RAM by moving allocations to external PSRAM.

## Quick Reference

### Total Memory Savings by Priority

| Priority | Component | Internal RAM Freed | Files to Modify | Effort | Risk |
|----------|-----------|-------------------|-----------------|--------|------|
| **CRITICAL** | Animation Decoders | **3-8 MB** | 4 | Low | Low |
| **HIGH** | Task Stacks (Phase 1) | **128 KB** | 4 | Medium | Low |
| **HIGH** | Channel Management | **150-500 KB** | 3 | Low | Low |
| **MEDIUM** | Task Stacks (Phase 2) | **70 KB** | 10 | Medium | Medium |
| **MEDIUM** | Display Rendering | **4 MB** | 1 | Medium | Medium |
| **LOW** | Network Buffers | **34-42 KB** | 2 | Very Low | Very Low |
| **LOW** | Miscellaneous | **15-30 KB** | 6 | Low | Very Low |
| **Total** | **All Components** | **7.5-12.7 MB** | 30 | - | - |

### Already Optimized (No Changes Needed) ✅

- GitHub OTA response buffers (128 KB)
- MQTT reassembly buffers (128 KB)
- Makapix artwork download (128 KB)
- Provisioning credentials (16 KB)
- Loader service file buffers (variable)
- PICO-8 frame buffers (128 KB)
- Makapix refresh task stack (12 KB)
- **Total already in SPIRAM**: ~540 KB ✅

### Keep in Internal RAM (Performance-Critical) ⚠️

- Upscale lookup tables (2.8 KB each, 4 total = ~11 KB)
- Upscale worker task stacks (4 KB total)
- DMA buffers (4-8 KB)
- **Total intentionally internal**: ~19-23 KB

---

## Implementation Phases

### Phase 1: Critical Impact - Animation Decoders (3-8 MB)

**Goal**: Move all image decoder frame buffers to SPIRAM

**Files to modify**:
1. `components/animation_decoder/webp_animation_decoder.c`
2. `components/animation_decoder/png_animation_decoder.c`
3. `components/animation_decoder/jpeg_animation_decoder.c`
4. `components/animated_gif_decoder/gif_animation_decoder.cpp`

**Changes**:
- Replace `malloc()` with `heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`
- Add fallback to `malloc()` if SPIRAM allocation fails
- ~20-30 lines total

**Testing**:
- Load and decode WebP, PNG, JPEG, GIF files
- Verify frame decode times (should be unchanged)
- Monitor internal RAM usage (should drop 3-8 MB)
- Test animation playback (should be smooth)

**Expected Impact**: **HIGHEST** - Frees 3-8 MB depending on loaded animations

**Detailed Documentation**: [01-ANIMATION-DECODERS.md](01-ANIMATION-DECODERS.md)

---

### Phase 2: High Impact - Large Task Stacks (128 KB)

**Goal**: Move 4 largest task stacks to SPIRAM

**Files to modify**:
1. `components/channel_manager/download_manager.c` - **80 KB** 🎯 Critical!
2. `components/makapix/makapix.c` - 16 KB (mqtt_reconn)
3. `main/animation_player.c` - 16 KB (anim_sd_refresh)
4. `components/makapix/makapix_provision_flow.c` - 16 KB (cred_poll)

**Changes**:
- Switch from `xTaskCreate()` to `xTaskCreateStatic()`
- Pre-allocate stacks with `heap_caps_malloc()`
- Add fallback to `xTaskCreate()` if SPIRAM unavailable
- ~50-60 lines total

**Testing**:
- Test download functionality
- Test MQTT connection/reconnection
- Test SD card refresh
- Test Makapix provisioning
- Monitor task execution (should be unchanged)

**Expected Impact**: **HIGH** - Frees 128 KB, especially download_mgr (80 KB)

**Detailed Documentation**: [05-TASK-STACKS.md](05-TASK-STACKS.md)

---

### Phase 3: High Impact - Channel Management (150-500 KB)

**Goal**: Move channel cache and playlist data structures to SPIRAM

**Files to modify**:
1. `components/channel_manager/channel_cache.c`
2. `components/channel_manager/playlist_manager.c`
3. `components/channel_manager/animation_metadata.c`

**Changes**:
- Move `cache->entries` array to SPIRAM (~148 KB)
- Move hash table nodes to SPIRAM (~16 KB)
- Move playlist artworks array to SPIRAM (~6-10 KB)
- Move JSON parsing buffers to SPIRAM (1-10 KB)
- ~15-25 lines total

**Testing**:
- Load large channel caches (1000+ entries)
- Test channel switching
- Test playlist loading
- Monitor metadata parsing speed

**Expected Impact**: **HIGH** - Frees 150-500 KB (scales with playlist size)

**Detailed Documentation**: [03-CHANNEL-MANAGEMENT.md](03-CHANNEL-MANAGEMENT.md)

---

### Phase 4: Medium Impact - Medium Task Stacks (70 KB)

**Goal**: Move remaining non-critical task stacks to SPIRAM

**Files to modify**: ~10 files
- `components/makapix/makapix.c` (ch_switch, makapix_prov)
- `components/ota_manager/ota_manager.c` (ota_check)
- `main/app_touch.c` (app_touch_task)
- `components/wifi_manager/app_wifi.c` (wifi_recovery, wifi_health, dns_server)
- `components/event_bus/event_bus.c` (event_bus)
- `components/makapix/makapix_connection.c` (status_pub)
- `components/http_api/http_api.c` (api_worker)
- `main/display_renderer.c` (display_render)
- `main/p3a_main.c` (mem_report)
- `components/play_scheduler/play_scheduler_refresh.c` (refresh_task)

**Changes**: Similar to Phase 2, ~50-70 lines total

**Testing**: Test each task's functionality individually

**Expected Impact**: **MEDIUM** - Frees ~70 KB

**Detailed Documentation**: [05-TASK-STACKS.md](05-TASK-STACKS.md)

---

### Phase 5: Medium Impact - Display Rendering (up to 4 MB)

**Goal**: Move native animation frame buffers to SPIRAM (if they exist)

**Files to modify**:
1. `main/animation_player_loader.c` (if native buffers are allocated there)

**Changes**:
- Locate native_frame_b1/b2 allocation
- Move to SPIRAM
- ~5-10 lines total

**Testing**:
- Test animation upscaling
- Monitor frame rendering performance
- Verify no stuttering during playback

**Expected Impact**: **MEDIUM-HIGH** - Up to 4 MB (needs verification)

**Detailed Documentation**: [02-DISPLAY-RENDERING.md](02-DISPLAY-RENDERING.md)

**Note**: Requires verification that these buffers exist and where they're allocated.

---

### Phase 6: Low Impact - Network Buffers (34-42 KB)

**Goal**: Move remaining HTTP/provisioning buffers to SPIRAM

**Files to modify**:
1. `components/http_api/http_api_rest.c` - JSON config buffer (32 KB)
2. `components/makapix/makapix_provision.c` - Setup buffer (2 KB)

**Changes**: ~5-10 lines total

**Testing**:
- Test HTTP `/config` and `/status` endpoints
- Test Makapix provisioning flow
- Monitor response times

**Expected Impact**: **LOW** - Frees 34-42 KB

**Detailed Documentation**: [04-NETWORK-BUFFERS.md](04-NETWORK-BUFFERS.md)

---

### Phase 7: Low Impact - Miscellaneous (15-30 KB)

**Goal**: Clean up remaining small allocations

**Files to modify**: 4-6 files (config_store, sync_playlist, slave_ota, wifi_manager)

**Changes**: ~10-20 lines total

**Testing**: Test each component individually

**Expected Impact**: **LOW** - Frees 15-30 KB

**Detailed Documentation**: [06-MISCELLANEOUS.md](06-MISCELLANEOUS.md)

---

## Implementation Guidelines

### Code Pattern for SPIRAM Allocation

```c
// Pattern 1: Simple malloc replacement
TYPE *ptr = (TYPE *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!ptr) {
    ptr = (TYPE *)malloc(size);  // Fallback to internal RAM
    if (!ptr) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes", size);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGW(TAG, "Using internal RAM (SPIRAM unavailable)");
}
```

```c
// Pattern 2: Task stack with xTaskCreateStatic
static StaticTask_t task_buffer;
StackType_t *task_stack = (StackType_t *)heap_caps_malloc(
    STACK_SIZE * sizeof(StackType_t),
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

if (task_stack) {
    TaskHandle_t handle = xTaskCreateStatic(
        task_func, "task_name", STACK_SIZE, NULL, priority,
        task_stack, &task_buffer);
    if (!handle) {
        free(task_stack);
        return ESP_FAIL;
    }
} else {
    // Fallback to dynamic allocation (internal RAM)
    BaseType_t ret = xTaskCreate(
        task_func, "task_name", STACK_SIZE, NULL, priority, &handle);
    if (ret != pdPASS) {
        return ESP_FAIL;
    }
}
```

### Testing Checklist

After each phase:
- [ ] Code compiles without warnings
- [ ] Component functionality unchanged
- [ ] No performance regressions
- [ ] Memory statistics show expected RAM savings
- [ ] No stability issues over 24-hour run
- [ ] Log shows SPIRAM allocations successful

### Memory Monitoring

Enable memory reporting in `menuconfig`:
```
P3A Features → General → Enable memory status reporting
```

This logs memory statistics every 15 seconds:
- Internal RAM free/total
- SPIRAM free/total
- Largest free blocks

**Expected Results After Phase 1**:
- Internal RAM free: +3-8 MB
- SPIRAM free: -3-8 MB

---

## Risk Mitigation

### Fallback Strategy

All SPIRAM allocations include fallback to internal RAM:
```c
ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!ptr) {
    ptr = malloc(size);  // Still works, just uses internal RAM
}
```

This ensures the system remains functional even if:
- SPIRAM is full
- SPIRAM allocation fails
- SPIRAM is disabled in configuration

### Performance Monitoring

SPIRAM is slower than internal RAM (~8-40× depending on access pattern). Monitor:
- Frame decode times (Phase 1)
- Animation rendering FPS (Phase 1, 5)
- Channel switching speed (Phase 3)
- Network response times (Phase 6)

If performance degrades:
1. Measure the specific operation
2. Profile to find bottleneck
3. Consider keeping that specific allocation internal
4. Document exception in code comments

### Rollback Plan

Each phase is independent and can be reverted:
1. Git revert the specific commit
2. Test that functionality is restored
3. Investigate root cause
4. Re-implement with fixes

---

## Success Metrics

### Memory Savings Targets

| Metric | Current | Target (All Phases) | Phase 1 Only |
|--------|---------|---------------------|--------------|
| Internal RAM free (idle) | ~X MB | ~X + 7-13 MB | ~X + 3-8 MB |
| SPIRAM usage | ~Y MB | ~Y + 7-13 MB | ~Y + 3-8 MB |
| Internal RAM usage (peak) | ~Z MB | ~Z - 5-10 MB | ~Z - 2-6 MB |
| OOM errors | >0? | 0 | Reduced |

### Performance Targets

| Metric | Current | Target | Acceptable Range |
|--------|---------|--------|------------------|
| WebP decode time | X ms | X ms | ±10% |
| Frame rate (720×720) | Y FPS | Y FPS | ≥Y FPS |
| Channel switch time | Z ms | Z ms | ±20% |
| HTTP response time | W ms | W ms | ±20% |

### Stability Targets

- **No regressions** in existing functionality
- **No new crashes** or freezes
- **No memory leaks** over 24-hour run
- **No SPIRAM exhaustion** under normal load

---

## Timeline Estimate

| Phase | Complexity | Estimated Time | Testing Time | Total |
|-------|------------|---------------|--------------|-------|
| Phase 1 | Low | 2-4 hours | 4-6 hours | **6-10 hours** |
| Phase 2 | Medium | 3-5 hours | 4-6 hours | **7-11 hours** |
| Phase 3 | Low | 2-3 hours | 3-4 hours | **5-7 hours** |
| Phase 4 | Medium | 4-6 hours | 6-8 hours | **10-14 hours** |
| Phase 5 | Medium | 2-4 hours | 4-6 hours | **6-10 hours** |
| Phase 6 | Low | 1-2 hours | 2-3 hours | **3-5 hours** |
| Phase 7 | Low | 1-2 hours | 2-3 hours | **3-5 hours** |
| **Total** | - | **15-26 hours** | **25-36 hours** | **40-62 hours** |

**Recommended**: Implement in **3-4 week sprints**, one phase per week, with thorough testing between phases.

---

## Next Steps

1. **Review this analysis** with the team
2. **Validate memory usage** with current build (baseline measurements)
3. **Start with Phase 1** (highest impact, lowest risk)
4. **Measure results** after each phase
5. **Iterate** based on findings

---

## Documentation Index

- [00-OVERVIEW.md](00-OVERVIEW.md) - Memory architecture and analysis summary
- [01-ANIMATION-DECODERS.md](01-ANIMATION-DECODERS.md) - WebP, PNG, JPEG, GIF decoders (Phase 1)
- [02-DISPLAY-RENDERING.md](02-DISPLAY-RENDERING.md) - Display buffers and rendering (Phase 5)
- [03-CHANNEL-MANAGEMENT.md](03-CHANNEL-MANAGEMENT.md) - Channels and playlists (Phase 3)
- [04-NETWORK-BUFFERS.md](04-NETWORK-BUFFERS.md) - HTTP, MQTT, OTA (Phase 6)
- [05-TASK-STACKS.md](05-TASK-STACKS.md) - FreeRTOS task stacks (Phases 2, 4)
- [06-MISCELLANEOUS.md](06-MISCELLANEOUS.md) - Other components (Phase 7)
- **ROADMAP.md** (this file) - Implementation plan

---

**Document Version**: 1.0  
**Date**: January 2026  
**Status**: Ready for Implementation  
**Author**: SPRAM Optimization Analysis

**Recommended First Step**: 🎯 **Start with Phase 1 (Animation Decoders)** - Highest impact, lowest risk!
