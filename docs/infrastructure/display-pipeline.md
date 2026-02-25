# Display Pipeline

## Initialization Flow

1. **Board init**: `p3a_board_display_init()` via BSP — configures MIPI-DSI, ST7703 controller
2. **Frame buffers**: Allocated in PSRAM (720x720x3 bytes each for RGB888, triple-buffered)
3. **Display renderer**: `display_renderer_init()` sets up vsync, buffer management

## Rendering Pipeline

```
┌──────────────┐     ┌────────────────┐     ┌──────────────┐
│   Decoder    │────>│ animation_     │────>│ Upscaler     │
│ (WebP/GIF/   │     │ player_render  │     │ (CPU or PPA) │
│  PNG/JPEG)   │     │ (composition,  │     └──────┬───────┘
└──────────────┘     │  aspect ratio) │            │
                     └────────────────┘            ▼
                                            ┌─────────────┐
                                            │ display_    │
                                            │ renderer    │
┌──────────────────────────┐                │ (vsync,     │
│ Overlays                 │───────────────>│  buffers)   │
│ - FPS counter            │                └──────┬──────┘
│ - Processing notification│                       │
└──────────────────────────┘                       ▼
                                            ┌─────────────┐
                                            │ LCD Panel   │
                                            │ DMA (DSI)   │
                                            └─────────────┘
```

## Rendering Features

- **Transparency/alpha blending**: Images with transparent backgrounds are composited over a configurable background color
- **Aspect ratio preservation**: Non-square images are scaled to fit while maintaining original proportions
- **Configurable background**: Background color can be set via web UI, REST API, or `render_engine_set_background()`
- **Channel-aware upscaling**: Giphy content uses PPA hardware-accelerated bilinear interpolation (smooth results for photographic/video content), while pixel art from Makapix and local files uses CPU nearest-neighbor scaling (crisp pixel edges). Configurable via settings.
- **Rotation**: 0/90/180/270 degree rotation, handled in both CPU and PPA upscale paths

## Upscaling Paths

### CPU Nearest-Neighbor (`display_upscaler.c`)
- Parallel upscaling using two FreeRTOS worker tasks (top/bottom halves)
- Handles rotation (0/90/180/270)
- Fills border regions with background color
- Cache coherency handling for PSRAM buffers

### PPA Hardware Bilinear (`display_ppa_upscaler.c`)
- Uses ESP32-P4 PPA (Pixel Processing Accelerator) SRM engine
- Bilinear interpolation for smooth upscaling
- Handles rotation and R/B channel swap in hardware
- Border regions filled via PPA Fill operation
- Conditional compilation: `CONFIG_P3A_PPA_UPSCALE_ENABLE`

## Overlays

### FPS Counter (`display_fps_overlay.c`)
- Renders FPS counter in the top-right corner using a 5x7 bitmap font (2x scaled)
- Tracks frame count and timing, displays up to 3 digits
- Respects config store enable/disable setting

### Processing Notification (`display_processing_notification.c`)
- Visual indicator for animation swap processing (blue checkerboard triangle) and failure (red triangle)
- State machine: IDLE → PROCESSING → FAILED → IDLE
- Drawn in the bottom-right corner, configurable size (16-256 px)
- 5-second timeout for processing, 3-second display for failure

## Key Files

| File | Purpose |
|------|---------|
| `display_renderer.c` | Frame buffer management, vsync, buffer lifecycle |
| `display_upscaler.c` | Parallel CPU nearest-neighbor upscaling with rotation and border fill |
| `display_ppa_upscaler.c` | PPA SRM bilinear upscaling with rotation (conditional) |
| `display_fps_overlay.c` | FPS counter overlay |
| `display_processing_notification.c` | Swap processing/failure visual indicator |
| `render_engine.c` | Display rotation and background color API |
| `animation_player_render.c` | Frame decoding, aspect ratio calculation, PPA/CPU upscale routing |
| `playback_controller.c` | Source switching (animation, PICO-8, UI) |
