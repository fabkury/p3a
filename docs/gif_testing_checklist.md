# GIF Playback Testing Checklist

## Pre-Test Verification

✅ **Component Integration**
- [x] AnimatedGIF library vendored in `firmware/components/animatedgif/`
- [x] GIF decoder adapter implemented with file I/O callbacks
- [x] Video player extended to support both WebP and GIF formats
- [x] Renderer updated to scan for `.gif` files
- [x] Format auto-detection implemented

✅ **Code Issues Fixed**
- [x] Added `strings.h` include for `strcasecmp`
- [x] Added `stdio.h` include for file operations
- [x] Fixed variable name consistency (`webp_decoder` vs `decoder`)
- [x] Added proper cleanup on error paths
- [x] Added frame buffer clearing on GIF reset

## Test Scenarios

### 1. Basic GIF Playback
- Place GIF file(s) in `/sdcard/animations/` directory
- Power on device
- Expected: GIF files discovered and included in animation cycle
- Monitor logs for: "Found animation: /sdcard/animations/xxx.gif"
- Monitor logs for: "Opened GIF: ... (width x height, loops=X)"

### 2. Format Detection
- Test with various file extensions (.gif, .GIF)
- Test with files without extensions (should use header detection)
- Expected: Format correctly detected and logged

### 3. Playback Performance
- Monitor log output every 30 frames:
  ```
  [VIDEO_PLAYER] Frame 30 complete: XX.X ms, avg fps=XX.X
  ```
- Compare GIF vs WebP performance
- Check for frame drops or stuttering

### 4. Memory Usage
- Monitor heap free during playback
- Large GIFs (>100MB) should not cause OOM
- Frame buffer allocated in SPIRAM for large GIFs

### 5. Error Handling
- Test with corrupted GIF files
- Test with very large GIF files
- Test with missing files
- Expected: Graceful error handling without crashes

### 6. Loop Behavior
- Test GIFs with loop count = 0 (infinite)
- Test GIFs with specific loop count
- Test with `should_loop` parameter
- Expected: Correct looping behavior

### 7. Format Switching
- Test cycling between WebP and GIF files
- Expected: Seamless switching without artifacts
- Monitor logs for: "Cycling from index X to Y"

## Known Issues to Watch For

1. **First Frame Delay**: First GIF frame may have incorrect timing
2. **Large GIFs**: Very large GIFs may experience stuttering on slow SD cards
3. **Frame Buffer**: Frame buffer cleared to black (0) instead of background color
4. **Disposal Methods**: Some complex disposal methods may not render perfectly

## Debug Logging

Key log tags to monitor:
- `video_player`: Main playback control
- `gif_decoder`: GIF decoding operations
- `renderer`: Animation file discovery and cycling

## Performance Baseline

Expected performance on ESP32-P4:
- WebP: ~25-30 FPS for 720x720 animations
- GIF: ~20-25 FPS for 720x720 animations (GIF decoding is CPU-intensive)
- Frame decode time: 10-30ms per frame depending on complexity
- DMA transfer time: ~2-5ms per stripe

## Next Steps After Testing

1. If GIFs don't play: Check file paths and format detection logs
2. If performance is poor: Consider enabling GIF Turbo mode (requires more RAM)
3. If memory issues: Reduce frame buffer size or optimize allocation
4. If disposal methods wrong: Review GIFDraw callback logic

