# GIF Playback Integration Documentation

## Overview

This document describes the GIF playback integration for the ESP32-P4 animation subsystem. The implementation extends the existing WebP animation playback to support GIF files using Larry Bank's AnimatedGIF library.

## Architecture

### Components

1. **animatedgif Component** (`firmware/components/animatedgif/`)
   - Vendored AnimatedGIF library from bitbank2/AnimatedGIF
   - Wrapper adapter (`gif_decoder.cpp/.h`) providing ESP-IDF compatible API
   - File I/O callbacks for SD card and SPIFFS integration

2. **video_player Component** (`firmware/components/video_player/`)
   - Unified animation playback API supporting both WebP and GIF
   - Format auto-detection based on file extension or header
   - Stripe-based DMA rendering for both formats

3. **renderer Component** (`firmware/components/renderer/`)
   - Extended to scan for both `.webp` and `.gif` files
   - Uses unified `video_player_play_file()` API for seamless format switching

### Integration Flow

```
SD Card/SPIFFS
    ↓
renderer scans for .webp and .gif files
    ↓
video_player_play_file() auto-detects format
    ↓
[WebP Path] → WebP decoder → stripe rendering → DMA
[GIF Path]  → GIF decoder → frame buffer → stripe rendering → DMA
```

## API Usage

### Basic Playback

```c
// Auto-detect format from file path
esp_err_t ret = video_player_play_file("/sdcard/animations/test.gif", true);

// Explicit format selection
esp_err_t ret = video_player_play_gif("/sdcard/animations/test.gif", true);

// WebP (backward compatible)
esp_err_t ret = video_player_play_webp(data, size, true);
```

### Playback Control

```c
// Pause/resume
video_player_pause();
video_player_resume();

// Stop
video_player_stop(false);  // Resume LVGL
video_player_stop(true);   // Keep bypass mode active
```

### Format Detection

```c
anim_format_t format = video_player_detect_format("/sdcard/animations/test.gif");
if (format == ANIM_FORMAT_GIF) {
    // Handle GIF
}
```

## Implementation Details

### GIF Decoder Adapter

The `gif_decoder` module provides a C API wrapper around the C++ AnimatedGIF class:

- **File I/O Callbacks**: `gifOpenFile()`, `gifCloseFile()`, `gifReadFile()`, `gifSeekFile()`
  - Implemented using standard `fopen()`/`fread()`/`fseek()` for SD/SPIFFS compatibility
  
- **Draw Callback**: `GIFDraw()`
  - Converts palette indices to RGB565
  - Handles transparency and disposal methods
  - Accumulates full frame into RGB565 buffer for stripe-based rendering

### Stripe-Based Rendering

GIF frames are decoded into a full RGB565 frame buffer in SPIRAM:

1. GIF decoder calls `GIFDraw()` for each scanline
2. Scanlines accumulate into full frame buffer
3. Frame buffer is synced to cache
4. Frame is rendered stripe-by-stripe to DMA-capable internal buffers
5. Stripes are sent to display via DMA

This approach allows handling very large GIFs (hundreds of MB) without loading entire file into memory.

### Memory Management

- **GIF Frame Buffer**: RGB565 frame buffer allocated in SPIRAM
- **Stripe Buffers**: Two ping-pong RGB565 buffers in DMA-capable internal SRAM
- **Incremental Reading**: GIF decoder reads from file incrementally via callbacks

### Performance Considerations

- **Frame Decoding**: GIF decoding is CPU-intensive; frame buffer caching reduces repeated decodes
- **Scaling**: Nearest-neighbor scaling implemented for non-native resolutions
- **Cache Synchronization**: SPIRAM frame buffers require cache sync before DMA access

## Limitations

1. **GIF Format Support**:
   - Supports standard GIF87a/GIF89a formats
   - Transparency and disposal methods supported
   - Loop count support (0 = infinite)
   - No support for GIF Turbo mode (optional enhancement)

2. **Memory Requirements**:
   - Full frame RGB565 buffer required (720x720x2 = ~1MB for native resolution)
   - Larger GIFs require proportionally more SPIRAM

3. **LVGL Mode**:
   - GIF files require video player bypass mode (LVGL suspended)
   - WebP files can use either LVGL mode or bypass mode

## Testing

### Test Files

Place test GIF files in `/sdcard/animations/` directory. The renderer will automatically discover and include them in the animation cycle.

### Benchmarking

Performance statistics are logged every 30 frames:
- Average FPS
- Frame decode time
- DMA transfer time
- Total frame time

Example log output:
```
[VIDEO_PLAYER] Frame 30 complete: 35.2 ms, avg fps=28.4
```

## Known Issues

1. **First Frame Delay**: First GIF frame may have incorrect timing due to decoder initialization
2. **Large GIFs**: Very large GIFs (>100MB) may experience stuttering on slower SD cards
3. **Format Detection**: Header-based detection may fail for non-standard GIF encodings

## Future Enhancements

1. **GIF Turbo Mode**: Enable AnimatedGIF turbo mode for 30x faster decoding (requires 48KB+ RAM)
2. **Unified Animation Factory**: Create factory pattern for format-specific optimizations
3. **Frame Skipping**: Implement frame skipping for slow decoders to maintain playback speed
4. **Hardware Acceleration**: Explore PPA (Pixel Processing Accelerator) for GIF scaling

## Dependencies

- `animatedgif` component (vendored AnimatedGIF library)
- `libwebp_decoder` component (for WebP support)
- `storage` component (for SD/SPIFFS file access)
- `esp_lcd` component (for display DMA)

## License

AnimatedGIF library is licensed under Apache License 2.0. See `firmware/components/animatedgif/LICENSE` for details.

