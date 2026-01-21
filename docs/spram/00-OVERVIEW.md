# SPRAM Memory Optimization Analysis for p3a

## Executive Summary

This analysis identifies opportunities to move memory allocations from **internal RAM** to **SPIRAM** (external PSRAM) in the p3a codebase. The goal is to preserve the precious internal SRAM for performance-critical operations while utilizing the abundant external memory for larger data structures.

## ESP32-P4 Memory Architecture

### Memory Types

1. **Internal RAM (SRAM)**
   - **Size**: ~500 KB total
   - **Speed**: Very fast (dual-port access, zero-wait-state)
   - **Use cases**: 
     - Real-time rendering buffers
     - DMA buffers
     - Small lookup tables
     - Task stacks for time-critical tasks
     - Performance-critical data structures
   - **Capability**: `MALLOC_CAP_INTERNAL` or `MALLOC_CAP_DMA`

2. **SPIRAM (External PSRAM)**
   - **Size**: 32 MB on ESP32-P4-WIFI6-Touch-LCD-4B
   - **Speed**: Slower (~8-40x slower than internal RAM)
   - **Use cases**:
     - Large buffers
     - Image/animation frame data
     - Network buffers
     - File buffers
     - Task stacks for non-critical tasks
     - Cache data structures
   - **Capability**: `MALLOC_CAP_SPIRAM`

3. **Terminology Note: SPRAM vs SPIRAM**
   - **SPIRAM** = Serial Peripheral Interface RAM (the correct term for ESP32-P4)
   - **SPRAM** = Non-standard term, likely referring to SPIRAM
   - This document uses **SPIRAM** consistently

### Current Configuration

From `sdkconfig`:
```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_SPEED_200M=y
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_SPIRAM_XIP_FROM_PSRAM=y
CONFIG_SPIRAM_FLASH_LOAD_TO_PSRAM=y
```

The system is configured to use SPIRAM for:
- Instruction fetching (XIP)
- Read-only data
- Flash content loading

## Memory Allocation Patterns Found

### Pattern A: Already Using SPIRAM (Good ✅)
Many components already use `heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` with fallback to internal RAM.

**Examples:**
- `pico8_render.c`: Frame buffers (131 KB)
- `makapix_mqtt.c`: MQTT reassembly buffer (128 KB)
- `github_ota.c`: GitHub API response buffer (128 KB)
- `makapix_artwork.c`: Download chunk buffer (128 KB)
- `loader_service.c`: Animation file buffer (variable)

### Pattern B: Using Internal RAM (Needs Review ⚠️)
Some allocations use `malloc()` or `calloc()` which defaults to internal RAM.

**Examples:**
- Animation decoders (WebP, PNG, JPEG, GIF): Frame buffers
- Channel cache: Entry arrays, post ID arrays
- Playlist manager: Artwork arrays
- Task stacks: Most task stacks use internal RAM
- HTTP/WebSocket: JSON buffers, response buffers

### Pattern C: Using Explicit Internal RAM (Intentional 🎯)
Some allocations explicitly use `MALLOC_CAP_INTERNAL` for performance reasons.

**Examples:**
- `animation_player_loader.c`: Upscale lookup tables (uint16_t arrays)
- `pico8_render.c`: Aspect ratio lookup tables

## Analysis by Component

See individual component analysis files:
- [01-ANIMATION-DECODERS.md](01-ANIMATION-DECODERS.md) - WebP, PNG, JPEG, GIF decoders
- [02-DISPLAY-RENDERING.md](02-DISPLAY-RENDERING.md) - Display renderer, frame buffers
- [03-CHANNEL-MANAGEMENT.md](03-CHANNEL-MANAGEMENT.md) - Channel cache, playlist manager
- [04-NETWORK-BUFFERS.md](04-NETWORK-BUFFERS.md) - HTTP, MQTT, OTA, downloads
- [05-TASK-STACKS.md](05-TASK-STACKS.md) - FreeRTOS task stack allocations
- [06-MISCELLANEOUS.md](06-MISCELLANEOUS.md) - Other components

## Recommendations Summary

### High Priority (Large Memory Savings)

1. **Animation Decoders**: Move all frame buffers to SPIRAM (~500 KB - 2 MB per animation)
2. **Channel Cache**: Move entry arrays and file buffers to SPIRAM (~100 KB - 1 MB)
3. **Task Stacks**: Move non-critical task stacks to SPIRAM (~50-100 KB)
4. **HTTP/WebSocket**: Move JSON and response buffers to SPIRAM (~32-64 KB)

### Medium Priority (Moderate Memory Savings)

5. **Playlist Manager**: Move artwork arrays to SPIRAM (~10-50 KB)
6. **Config Store**: Move JSON serialization buffers to SPIRAM (~1-5 KB)
7. **Metadata Parsing**: Move temporary buffers to SPIRAM (~1-10 KB)

### Low Priority (Small Memory Savings, Keep Internal for Performance)

8. **Lookup Tables**: Keep in internal RAM for fast access (<10 KB total)
9. **DMA Buffers**: Must remain in DMA-capable memory
10. **Time-Critical Structures**: Keep in internal RAM

## Expected Impact

By implementing the high-priority recommendations:
- **Internal RAM freed**: ~650 KB - 3.1 MB (depending on loaded content)
- **SPIRAM usage increase**: Same amount (32 MB available, currently underutilized)
- **Performance impact**: Minimal (SPIRAM speed adequate for buffers)
- **Stability improvement**: Reduced internal RAM pressure, fewer OOM errors

## Implementation Strategy

1. **Phase 1**: Animation decoders (highest impact)
2. **Phase 2**: Channel management and cache
3. **Phase 3**: Task stacks for non-critical tasks
4. **Phase 4**: HTTP/WebSocket buffers
5. **Phase 5**: Miscellaneous small buffers

Each phase should include:
- Code changes with `heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`
- Fallback to `malloc()` if SPIRAM allocation fails
- Testing for memory usage and performance
- Monitoring for any stability issues

## Tools for Monitoring

The codebase already includes memory monitoring in `p3a_main.c`:
```c
CONFIG_P3A_MEMORY_REPORTING_ENABLE
```

When enabled, logs memory statistics every 15 seconds:
- Internal RAM free/total
- SPIRAM free/total
- DMA-capable RAM free/total
- Largest free block

## Next Steps

1. Review each component analysis file for specific recommendations
2. Prioritize changes based on memory impact and risk
3. Implement changes in phases
4. Test each phase thoroughly before proceeding
5. Monitor memory usage and system stability

---

**Document Version**: 1.0  
**Date**: January 2026  
**Status**: Initial Analysis
