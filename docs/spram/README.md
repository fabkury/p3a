# SPRAM Memory Optimization Analysis

This directory contains comprehensive analysis of memory allocation opportunities to optimize internal RAM usage by moving allocations to SPIRAM (external PSRAM) on the ESP32-P4.

## Quick Start

**New to this analysis?** Start here:
1. Read [00-OVERVIEW.md](00-OVERVIEW.md) for memory architecture overview
2. Read [ROADMAP.md](ROADMAP.md) for implementation plan
3. Jump to phase documentation for details

## Document Index

### Overview and Planning
- **[00-OVERVIEW.md](00-OVERVIEW.md)** - Memory architecture, terminology, and summary
- **[ROADMAP.md](ROADMAP.md)** - Prioritized implementation roadmap ⭐ **START HERE**

### Component Analysis (by Priority)

#### Critical Impact (3-8 MB savings)
- **[01-ANIMATION-DECODERS.md](01-ANIMATION-DECODERS.md)** - WebP, PNG, JPEG, GIF frame buffers

#### High Impact (128-500 KB savings)
- **[05-TASK-STACKS.md](05-TASK-STACKS.md)** - FreeRTOS task stack allocations
- **[03-CHANNEL-MANAGEMENT.md](03-CHANNEL-MANAGEMENT.md)** - Channel cache and playlist data

#### Medium Impact (up to 4 MB savings)
- **[02-DISPLAY-RENDERING.md](02-DISPLAY-RENDERING.md)** - Display buffers and rendering (needs verification)

#### Low Impact (34-70 KB savings)
- **[04-NETWORK-BUFFERS.md](04-NETWORK-BUFFERS.md)** - HTTP, MQTT, OTA buffers (mostly optimized ✅)
- **[06-MISCELLANEOUS.md](06-MISCELLANEOUS.md)** - Config, sync, and other components

## Summary Statistics

### Total Potential Savings: 7.5 - 12.7 MB

| Category | Internal RAM Freed | Status |
|----------|-------------------|--------|
| Animation Decoders | 3-8 MB | ⚠️ Needs optimization |
| Task Stacks | 198 KB | ⚠️ Needs optimization |
| Channel Management | 150-500 KB | ⚠️ Needs optimization |
| Display Rendering | up to 4 MB | ⚠️ Needs verification |
| Network Buffers | 34-42 KB | Mostly ✅ optimized |
| Miscellaneous | 15-30 KB | ⚠️ Needs optimization |

### Already Optimized: ~540 KB ✅

The codebase already has excellent SPIRAM usage for:
- GitHub OTA buffers (128 KB)
- MQTT reassembly (128 KB)
- Makapix downloads (128 KB)
- PICO-8 frame buffers (128 KB)
- Provisioning credentials (16 KB)
- Makapix task stack (12 KB)

## Implementation Phases

See [ROADMAP.md](ROADMAP.md) for detailed phased implementation plan.

**Quick Reference:**
1. **Phase 1 (CRITICAL)**: Animation decoders → 3-8 MB freed
2. **Phase 2 (HIGH)**: Large task stacks → 128 KB freed
3. **Phase 3 (HIGH)**: Channel management → 150-500 KB freed
4. **Phase 4 (MEDIUM)**: Medium task stacks → 70 KB freed
5. **Phase 5 (MEDIUM)**: Display rendering → up to 4 MB freed
6. **Phase 6 (LOW)**: Network buffers → 34-42 KB freed
7. **Phase 7 (LOW)**: Miscellaneous → 15-30 KB freed

## Key Findings

### ✅ What's Working Well

1. **Network buffers** are already using SPIRAM for large allocations
2. **Fallback patterns** are in place (SPIRAM → internal RAM)
3. **PICO-8 rendering** correctly uses SPIRAM for frame buffers
4. **Makapix channel** uses SPIRAM for task stack

### ⚠️ Opportunities for Improvement

1. **Animation decoders** use `malloc()` for large frame buffers (3-8 MB)
2. **Task stacks** default to internal RAM (198 KB total)
3. **Channel cache** uses internal RAM for large arrays (150-500 KB)
4. **Display native buffers** may use internal RAM (up to 4 MB - needs verification)

### 🎯 Performance-Critical (Keep Internal)

1. **Upscale lookup tables** (2.8 KB each) - frequently accessed
2. **Upscale worker tasks** (4 KB total) - core-pinned, real-time
3. **DMA buffers** (4-8 KB) - hardware requirement

## Terminology

- **Internal RAM (SRAM)**: Fast (~500 KB), precious, use for performance-critical data
- **SPIRAM (External PSRAM)**: Slow but abundant (32 MB), use for large buffers
- **SPRAM**: Non-standard term, likely means SPIRAM in this context
- **MALLOC_CAP_INTERNAL**: Allocate from internal RAM
- **MALLOC_CAP_SPIRAM**: Allocate from external PSRAM
- **MALLOC_CAP_DMA**: Allocate from DMA-capable memory

## Testing Strategy

After implementing changes:
1. ✅ Code compiles without warnings
2. ✅ Component functionality unchanged
3. ✅ No performance regressions
4. ✅ Memory statistics show expected savings
5. ✅ No stability issues over 24-hour run
6. ✅ Log shows successful SPIRAM allocations

Enable memory reporting in `menuconfig`:
```
P3A Features → General → Enable memory status reporting
```

## Code Patterns

### Pattern 1: Simple Allocation
```c
TYPE *ptr = (TYPE *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!ptr) {
    ptr = (TYPE *)malloc(size);  // Fallback to internal RAM
}
```

### Pattern 2: Task Stack (Static)
```c
static StaticTask_t task_buffer;
StackType_t *stack = heap_caps_malloc(STACK_SIZE * sizeof(StackType_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (stack) {
    handle = xTaskCreateStatic(func, "name", STACK_SIZE, NULL, priority, 
                               stack, &task_buffer);
} else {
    xTaskCreate(func, "name", STACK_SIZE, NULL, priority, &handle);
}
```

## Contributing

When adding new code:
- Large buffers (> 10 KB): Use SPIRAM first
- Small, frequently accessed: Keep internal
- DMA buffers: Use MALLOC_CAP_DMA
- Task stacks (non-critical): Use SPIRAM
- Read-only data: Use `const` for flash storage

See [06-MISCELLANEOUS.md](06-MISCELLANEOUS.md) for best practices checklist.

## Questions?

- **What is SPRAM?** → See [00-OVERVIEW.md](00-OVERVIEW.md#esp32-p4-memory-architecture)
- **Where to start?** → See [ROADMAP.md](ROADMAP.md#phase-1-critical-impact---animation-decoders-3-8-mb)
- **How to test?** → See [ROADMAP.md](ROADMAP.md#testing-checklist)
- **What about performance?** → SPIRAM is adequate for buffers, see individual analyses

## License

This analysis is part of the p3a project and follows the same license (Apache-2.0).

---

**Last Updated**: January 2026  
**Status**: Analysis Complete - Ready for Implementation  
**Next Step**: 🎯 **Implement Phase 1 (Animation Decoders)** for maximum impact!
