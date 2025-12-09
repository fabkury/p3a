# PIE SIMD Optimization for Vertical Row Duplication

## Overview

This optimization uses ESP32-P4 PIE (SIMD) instructions to accelerate vertical upscaling by detecting and copying duplicate rows instead of re-rendering them.

## Problem

During nearest-neighbor upscaling (e.g., 240×240 → 720×720), multiple destination rows often map to the same source row. In the 3x upscale example, each source row produces 3 identical destination rows. The original implementation rendered each destination row independently, performing redundant pixel conversions and lookups.

## Solution

### Vertical Run Detection

The optimization detects "runs" of consecutive destination rows that map to the same source row:

```c
// Example: source row 10 maps to destination rows 30, 31, 32
for (dst_y = 30; dst_y < 33; dst_y++) {
    // lookup_y[30] == lookup_y[31] == lookup_y[32] == 10
}
```

### Render + Copy Strategy

1. **Render first row**: Use existing scalar code to render the first row of each run
2. **Copy duplicates**: Use `pie_memcpy_128()` to copy that row to all subsequent rows in the run

```c
// Render destination row 30 (first of the run)
render_row(dst_buffer, row=30, src_row=10);

// Copy row 30 to rows 31 and 32 using PIE SIMD
for (int dy = 31; dy < 33; dy++) {
    pie_memcpy_128(dst_buffer + dy * stride, 
                   dst_buffer + 30 * stride, 
                   stride);
}
```

## PIE memcpy Implementation

The `pie_memcpy_128()` function uses ESP32-P4 PIE 128-bit vector instructions:

- **128-byte blocks**: Uses `esp.vld.128.ip` and `esp.vst.128.ip` with hardware loops (HWLP)
  - Loads/stores 8x 16-byte vectors per iteration
- **16-byte tail**: Uses HWLP for remaining 16-byte chunks
- **Byte tail**: Uses scalar byte-by-byte copy for final 0-15 bytes

### Performance Characteristics

- **Best case**: Row size is multiple of 128 bytes (pure vector ops)
- **Good case**: Row size is multiple of 16 bytes (vector + small tail)
- **Works case**: Any row size (handles non-aligned and non-multiple sizes)

For a 720×720 RGB565 framebuffer:
- Row stride: 1440 bytes (720 pixels × 2 bytes/pixel)
- 1440 = 11×128 + 32 = 11 vector blocks + 2 vector chunks

## Configuration

### Enable (default)
```
CONFIG_P3A_USE_PIE_SIMD=y
```
- Uses PIE SIMD for ROTATION_0 vertical runs
- Falls back to scalar for other rotations

### Disable
```
CONFIG_P3A_USE_PIE_SIMD=n
```
- Uses scalar rendering for all cases
- No PIE assembly compiled into firmware
- Useful for debugging or if PIE causes issues

## Scope and Limitations

### What is optimized
- ✅ ROTATION_0 vertical duplication
- ✅ Compile-time optional
- ✅ Handles any row size

### What is NOT changed
- ❌ ROTATION_90, ROTATION_180, ROTATION_270 (use scalar path)
- ❌ Horizontal upscaling (already efficient with lookup tables)
- ❌ Color conversion (still done per-pixel)
- ❌ First row of each run (rendered with scalar code)

## Code Changes Summary

### Files Added
- `components/p3a_core/pie_memcpy_128.S` - PIE assembly implementation
- `components/p3a_core/include/pie_memcpy_128.h` - C interface

### Files Modified
- `main/Kconfig.projbuild` - Added CONFIG_P3A_USE_PIE_SIMD option
- `main/display_renderer.c` - Added vertical run detection and PIE memcpy calls
- `components/p3a_core/CMakeLists.txt` - Conditionally compile assembly

### Behavioral Guarantees
- ✅ **Bit-exact output**: Same pixels as before (verified by design)
- ✅ **No visual changes**: Only performance improvement
- ✅ **Backward compatible**: Can be disabled via Kconfig

## Future Enhancements

Possible extensions (not implemented in this PR):
1. **Other rotations**: Detect horizontal runs for ROTATION_90/270
2. **Full-frame dedup**: Hash rows to detect duplicates across entire frame
3. **Alignment enforcement**: Ensure 16-byte alignment for dst_buffer allocation
4. **Row caching**: Keep a pool of pre-rendered rows for common patterns

## References

- ESP32-P4 PIE instructions: `esp.vld.128.ip`, `esp.vst.128.ip`
- ESP32-P4 HWLP: `esp.lp.setup` for zero-overhead loops
- RISC-V calling convention: a0=dst, a1=src, a2=len
