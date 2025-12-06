# P3A Animation Playback and Control Pipeline - Technical Report

## Executive Summary

This document provides a comprehensive technical analysis of the p3a (Pixel Pea) animation playback and control pipeline, including the channels abstraction. This report covers the complete flow from SD card file discovery to frame rendering on the LCD display, explaining each architectural component and their interactions.

**Target Audience**: Developers who need to understand or extend the animation playback functionality, particularly the channels system.

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Channel System Architecture](#channel-system-architecture)
3. [Animation Player Core](#animation-player-core)
4. [Decoder Pipeline](#decoder-pipeline)
5. [Render Loop and Buffer Management](#render-loop-and-buffer-management)
6. [Threading Model](#threading-model)
7. [Data Flow Diagrams](#data-flow-diagrams)
8. [Key API Reference](#key-api-reference)
9. [Extension Points](#extension-points)

---

## 1. Architecture Overview

The p3a animation system is built on a multi-layered architecture with clear separation of concerns:

```
┌────────────────────────────────────────────────────────────┐
│                    User Input Layer                         │
│  (Touch, Web UI, REST API, MQTT Commands)                   │
└───────────────────────┬────────────────────────────────────┘
                        │
┌───────────────────────▼────────────────────────────────────┐
│              Animation Player (animation_player.c)          │
│  - Initialization and lifecycle management                  │
│  - Public API for control (pause, cycle, load)              │
│  - Mode switching (Animation ↔ UI)                          │
└───────┬───────────────┬────────────────┬───────────────────┘
        │               │                │
        │          ┌────▼────┐      ┌────▼────────┐
        │          │ Channel │      │   Render    │
        │          │ Player  │      │   Loop      │
        │          └────┬────┘      └────┬────────┘
        │               │                │
┌───────▼───────┐  ┌────▼────────┐  ┌───▼─────────┐
│ Loader Task   │  │   SD Card   │  │  Upscale    │
│ (Background)  │  │   Channel   │  │  Workers    │
└───────┬───────┘  └────┬────────┘  └───┬─────────┘
        │               │                │
        │          ┌────▼────────┐       │
        │          │  File       │       │
        │          │  System     │       │
        │          └─────────────┘       │
        │                                │
┌───────▼────────────────────────────────▼───────────┐
│           Decoder Pipeline                          │
│  (WebP, GIF, PNG, JPEG decoders)                    │
└───────────────────────┬─────────────────────────────┘
                        │
┌───────────────────────▼─────────────────────────────┐
│              LCD Display Hardware                    │
│  (720×720 IPS, DMA, multi-buffer, VSync)            │
└─────────────────────────────────────────────────────┘
```

### Core Components

1. **Channel System** (`sdcard_channel.c`, `channel_player.c`)
   - Abstracts content source (SD card)
   - Manages post discovery and metadata
   - Provides pagination and sorting

2. **Animation Player** (`animation_player.c`, `animation_player_loader.c`, `animation_player_render.c`)
   - Orchestrates playback
   - Manages double-buffering
   - Controls render modes

3. **Decoder Pipeline** (`animation_decoder.h`, format-specific decoders)
   - Unified interface for multiple formats
   - Frame-by-frame decoding
   - Format-agnostic API

4. **Render System** (`animation_player_render.c`)
   - High-performance upscaling
   - Multi-threaded rendering
   - VSync synchronization

---

## 2. Channel System Architecture

The channel system is a three-tier abstraction that separates content storage, content management, and playback control.

### 2.1 Architecture Layers

```
┌─────────────────────────────────────────────────────┐
│                  Channel Player                      │
│  - Playback sequencing (advance, go_back)            │
│  - Randomization control                             │
│  - Position tracking                                 │
│  - Post selection for playback                       │
└───────────────────────┬─────────────────────────────┘
                        │ Uses
┌───────────────────────▼─────────────────────────────┐
│                  SD Card Channel                     │
│  - Post enumeration from filesystem                  │
│  - Metadata extraction (type, date, path)            │
│  - Sorting (by name or date)                         │
│  - Pagination support                                │
│  - Health tracking                                   │
└───────────────────────┬─────────────────────────────┘
                        │ Reads from
┌───────────────────────▼─────────────────────────────┐
│              SD Card Filesystem                      │
│  - /sdcard/animations/ directory                     │
│  - WebP, GIF, PNG, JPEG files                        │
└─────────────────────────────────────────────────────┘
```

### 2.2 SD Card Channel (`sdcard_channel.c`)

**Purpose**: Content discovery and metadata management for SD card storage.

**Key Data Structures**:
```c
typedef struct {
    char *name;           // Filename without path
    time_t created_at;    // File creation timestamp
    char *filepath;       // Full path for loading
    asset_type_t type;    // WEBP, GIF, PNG, JPEG
    bool healthy;         // Load health flag
} sdcard_post_t;
```

**Capabilities**:
- Scans `/sdcard/animations/` directory for supported files
- Extracts file metadata (name, timestamp, type)
- Maintains up to 1000 posts in memory (`SDCARD_CHANNEL_MAX_POSTS`)
- Supports two sort orders: by name (alphabetical) or by date (newest first)
- Provides paginated queries (max 50 results per page)
- Tracks file health (failed loads marked as unhealthy)

**Key Functions**:
```c
// Initialize the channel system
esp_err_t sdcard_channel_init(void);

// Scan filesystem and populate post list
esp_err_t sdcard_channel_refresh(const char *animations_dir);

// Query posts with pagination and sorting
esp_err_t sdcard_channel_query(const sdcard_query_t *query, 
                                sdcard_query_result_t *result);

// Get total post count
size_t sdcard_channel_get_total_count(void);

// Mark post as failed to load
void sdcard_channel_mark_unhealthy(size_t post_index);
```

**Implementation Details**:
- Posts are allocated dynamically and stored in internal array
- Sorting is lazy: only re-sorts when requested order differs from current
- Query results allocate new arrays (caller must free)
- File type detection is case-insensitive based on extension

### 2.3 Channel Player (`channel_player.c`)

**Purpose**: Manages playback sequencing and post selection.

**Key Data Structures**:
```c
typedef struct {
    sdcard_post_t *posts;  // Loaded posts (up to 1000)
    size_t *indices;       // Playback order indices
    size_t count;          // Number of loaded posts
    size_t current_pos;    // Current position in playback order
    bool randomize;        // Randomization enabled
} channel_player_state_t;
```

**Capabilities**:
- Loads posts from SD card channel (most recent first)
- Maintains playback order separate from storage order
- Supports randomization using Fisher-Yates shuffle
- Provides sequential or random playback
- Automatically wraps at end of list
- Re-shuffles on wrap when randomization enabled

**Key Functions**:
```c
// Load posts from channel for playback
esp_err_t channel_player_load_channel(void);

// Get current post for playback
esp_err_t channel_player_get_current_post(const sdcard_post_t **out_post);

// Advance to next post
esp_err_t channel_player_advance(void);

// Go back to previous post
esp_err_t channel_player_go_back(void);

// Enable/disable randomization
void channel_player_set_randomize(bool enable);
```

**Implementation Details**:
- Posts are shallow-copied from channel (shares string pointers)
- Indices array enables O(1) random access with reordering
- Fisher-Yates shuffle guarantees uniform distribution
- Randomization setting persists across loads
- Default mode is sequential (randomize = false)

### 2.4 Channel System Data Flow

```
Initialization Flow:
─────────────────

1. animation_player_init()
   ↓
2. sdcard_channel_init()
   - Allocates channel state
   ↓
3. sdcard_channel_refresh("/sdcard/animations")
   - Scans directory
   - Creates post entries
   - Sorts by date (newest first)
   ↓
4. channel_player_init()
   - Allocates player state
   ↓
5. channel_player_load_channel()
   - Queries channel with pagination
   - Copies posts (up to 1000)
   - Initializes indices array
   - Optionally shuffles


Playback Flow:
──────────────

1. channel_player_get_current_post()
   - Returns &posts[indices[current_pos]]
   ↓
2. animation_player loads file from post->filepath
   ↓
3. User triggers "next" or auto-advance timer
   ↓
4. channel_player_advance()
   - current_pos++
   - If at end: wrap to 0, optionally re-shuffle
   ↓
5. Loop back to step 1
```

### 2.5 Extension Points for Channel System

**Adding New Content Sources**:
To add support for network-based channels (e.g., cloud storage, streaming):

1. Create new channel module (e.g., `cloud_channel.c`)
2. Implement same interface as `sdcard_channel.h`:
   - `init()`, `refresh()`, `query()`, `get_post()`
3. Update `channel_player.c` to support multiple channel types
4. Add channel type selection to configuration

**Potential Design**:
```c
typedef enum {
    CHANNEL_TYPE_SDCARD,
    CHANNEL_TYPE_CLOUD,
    CHANNEL_TYPE_STREAMING
} channel_type_t;

typedef struct {
    esp_err_t (*refresh)(void *ctx, const char *source);
    esp_err_t (*query)(void *ctx, query_t *q, result_t *r);
    void *context;
} channel_vtable_t;
```

**Adding Metadata Fields**:
To add more metadata (e.g., artist, tags, favorites):

1. Extend `sdcard_post_t` structure
2. Update `sdcard_channel_refresh()` to extract new metadata
3. Add new sort orders if needed
4. Update query filters

---

## 3. Animation Player Core

### 3.1 Overview

The animation player (`animation_player.c`) is the central orchestrator that manages:
- Initialization and resource allocation
- Channel system lifecycle
- Double-buffered animation loading
- Render mode switching (Animation ↔ UI)
- Public control API

### 3.2 Key Data Structures

**Animation Buffer**:
```c
typedef struct {
    animation_decoder_t *decoder;        // Decoder instance
    const uint8_t *file_data;            // Memory-mapped file data
    size_t file_size;                    // File size in bytes
    animation_decoder_info_t decoder_info; // Canvas size, frame count
    asset_type_t type;                   // Format type
    size_t asset_index;                  // Post index from channel
    
    // Native resolution buffers (RGBA decoded frames)
    uint8_t *native_frame_b1;            // Double buffer 1
    uint8_t *native_frame_b2;            // Double buffer 2
    uint8_t native_buffer_active;        // Active buffer (0 or 1)
    size_t native_frame_size;            // Buffer size
    
    // Upscaling lookup tables
    uint16_t *upscale_lookup_x;          // X coordinate mapping
    uint16_t *upscale_lookup_y;          // Y coordinate mapping
    int upscale_src_w, upscale_src_h;    // Source dimensions
    int upscale_dst_w, upscale_dst_h;    // Target dimensions
    
    // Prefetch optimization
    uint8_t *prefetched_first_frame;     // Pre-rendered first frame
    bool first_frame_ready;              // Prefetch complete flag
    bool decoder_at_frame_1;             // Decoder position
    bool prefetch_pending;               // Prefetch request flag
    uint32_t prefetched_first_frame_delay_ms; // Frame duration
    uint32_t current_frame_delay_ms;     // Current frame duration
    
    bool ready;                          // Buffer ready for swap
} animation_buffer_t;
```

**Global State**:
```c
// Display resources
esp_lcd_panel_handle_t s_display_handle;  // LCD panel
uint8_t **s_lcd_buffers;                  // Triple-buffered LCD memory
uint8_t s_buffer_count;                   // Number of LCD buffers (3)
size_t s_frame_buffer_bytes;              // Size per buffer
size_t s_frame_row_stride_bytes;          // Row stride

// Synchronization
SemaphoreHandle_t s_vsync_sem;            // VSync notification
SemaphoreHandle_t s_buffer_mutex;         // Protects buffer state
SemaphoreHandle_t s_loader_sem;           // Loader task signal

// Tasks
TaskHandle_t s_anim_task;                 // Render loop task
TaskHandle_t s_loader_task;               // Background loader
TaskHandle_t s_upscale_worker_top;        // Top half upscaler
TaskHandle_t s_upscale_worker_bottom;     // Bottom half upscaler

// Double buffering
animation_buffer_t s_front_buffer;        // Currently rendering
animation_buffer_t s_back_buffer;         // Loading next animation
bool s_swap_requested;                    // Swap pending flag

// Playback control
bool s_anim_paused;                       // Pause state
render_mode_t s_render_mode_request;      // Requested mode
render_mode_t s_render_mode_active;       // Active mode
```

### 3.3 Initialization Flow

```
animation_player_init() execution:
──────────────────────────────────

1. Store LCD resources (handle, buffers, dimensions)
   ↓
2. Initialize VSync semaphore and register callback
   ↓
3. Initialize channel system:
   - sdcard_channel_init()
   - channel_player_init()
   ↓
4. Mount SD card and discover animations directory
   - Tries /sdcard/animations first (preferred)
   - Falls back to searching SD card
   ↓
5. Refresh channel (enumerate files)
   - sdcard_channel_refresh()
   ↓
6. Load posts into channel player
   - channel_player_load_channel()
   ↓
7. Create synchronization primitives
   - Buffer mutex
   - Loader semaphore
   ↓
8. Load first animation into front buffer
   - load_first_animation() → channel_player_get_current_post()
   - load_animation_into_buffer()
   ↓
9. Create worker tasks:
   - upscale_worker_top (core 0)
   - upscale_worker_bottom (core 0)
   - animation_loader_task (core 1)
   ↓
10. Return ESP_OK (ready for animation_player_start())
```

### 3.4 Loading Pipeline

**File Loading** (`animation_player_loader.c`):
```c
esp_err_t load_animation_into_buffer(const char *filepath, 
                                     asset_type_t type, 
                                     animation_buffer_t *buf)
```

Steps:
1. Open file and get size
2. Allocate SPIRAM buffer for entire file
3. Read file into memory (memory-mapped approach)
4. Initialize appropriate decoder based on `type`
5. Get decoder info (canvas size, frame count)
6. Allocate native-resolution RGBA buffers (double-buffered)
7. Create upscaling lookup tables for bilinear interpolation
8. Allocate prefetch buffer for first frame
9. Mark buffer as ready

**Background Loader Task**:
```c
void animation_loader_task(void *arg)
```

The loader task runs continuously in background:
1. Waits on `s_loader_sem`
2. When signaled, gets current post from channel player
3. Loads animation into back buffer
4. Sets `prefetch_pending = true`
5. Signals render task to handle prefetch

This enables smooth transitions: while one animation plays, the next loads in background.

### 3.5 Control API

**Playback Control**:
```c
// Pause/unpause
void animation_player_set_paused(bool paused);
void animation_player_toggle_pause(void);
bool animation_player_is_paused(void);

// Navigation
void animation_player_cycle_animation(bool forward);
```

**Cycle Implementation**:
```c
void animation_player_cycle_animation(bool forward) {
    if (forward) {
        channel_player_advance();
    } else {
        channel_player_go_back();
    }
    
    // Signal loader task to load new animation
    s_swap_requested = true;
    xSemaphoreGive(s_loader_sem);
}
```

**Mode Switching**:
```c
// Enter UI mode (for menus, settings)
esp_err_t animation_player_enter_ui_mode(void);

// Exit UI mode (resume animations)
void animation_player_exit_ui_mode(void);
```

Mode switching is implemented with request/acknowledge handshake:
1. Caller sets `s_render_mode_request = RENDER_MODE_UI`
2. Waits for `s_render_mode_active == RENDER_MODE_UI`
3. Render task acknowledges mode change on next frame

---

## 4. Decoder Pipeline

### 4.1 Unified Decoder Interface

The decoder pipeline provides a format-agnostic API for frame-by-frame decoding.

**Abstract Interface** (`animation_decoder.h`):
```c
typedef struct animation_decoder_s animation_decoder_t;

typedef enum {
    ANIMATION_DECODER_TYPE_WEBP,
    ANIMATION_DECODER_TYPE_GIF,
    ANIMATION_DECODER_TYPE_PNG,
    ANIMATION_DECODER_TYPE_JPEG,
} animation_decoder_type_t;

typedef struct {
    uint32_t canvas_width;
    uint32_t canvas_height;
    size_t frame_count;
    bool has_transparency;
} animation_decoder_info_t;
```

**Core API**:
```c
// Create decoder instance
esp_err_t animation_decoder_init(animation_decoder_t **decoder,
                                animation_decoder_type_t type,
                                const uint8_t *data,
                                size_t size);

// Get animation info
esp_err_t animation_decoder_get_info(animation_decoder_t *decoder,
                                    animation_decoder_info_t *info);

// Decode next frame to RGBA buffer
esp_err_t animation_decoder_decode_next(animation_decoder_t *decoder,
                                       uint8_t *rgba_buffer);

// Get frame duration in milliseconds
esp_err_t animation_decoder_get_frame_delay(animation_decoder_t *decoder,
                                           uint32_t *delay_ms);

// Reset to beginning
esp_err_t animation_decoder_reset(animation_decoder_t *decoder);

// Free resources
void animation_decoder_unload(animation_decoder_t **decoder);
```

### 4.2 Internal Structure

**Implementation** (`animation_decoder_internal.h`):
```c
struct animation_decoder_s {
    animation_decoder_type_t type;
    union {
        struct {
            void *decoder;     // WebPAnimDecoder*
            void *info;        // WebPAnimInfo*
            const uint8_t *data;
            size_t data_size;
            bool initialized;
        } webp;
        struct {
            void *gif_decoder;
        } gif;
        struct {
            void *png_decoder;
        } png;
        struct {
            void *jpeg_decoder;
        } jpeg;
    } impl;
};
```

**Format-Specific Implementations**:
- `libwebp_decoder` component: WebP (animated)
- `animated_gif_decoder` component: GIF (via AnimatedGIF library)
- `png_animation_decoder.c`: PNG (static images, treated as single-frame)
- `jpeg_animation_decoder.c`: JPEG (static images, treated as single-frame)

### 4.3 Decoder Lifecycle

```
Initialization:
──────────────
1. animation_decoder_init(ANIMATION_DECODER_TYPE_WEBP, data, size)
   ↓
2. Allocate decoder structure
   ↓
3. Initialize format-specific decoder (e.g., WebPAnimDecoderNew)
   ↓
4. Query metadata (dimensions, frame count)
   ↓
5. Return decoder handle


Decoding Loop:
─────────────
1. animation_decoder_decode_next(decoder, rgba_buffer)
   ↓
2. Decode current frame to RGBA format
   ↓
3. Internal decoder advances to next frame
   ↓
4. Return ESP_OK or ESP_ERR_INVALID_STATE (at end)
   ↓
5. animation_decoder_get_frame_delay(decoder, &delay_ms)
   ↓
6. Return frame duration for timing


Reset:
──────
1. animation_decoder_reset(decoder)
   ↓
2. Rewind internal decoder to beginning
   ↓
3. Return ESP_OK


Cleanup:
────────
1. animation_decoder_unload(&decoder)
   ↓
2. Free format-specific resources
   ↓
3. Free decoder structure
   ↓
4. Set pointer to NULL
```

### 4.4 Output Format

All decoders output **32-bit RGBA** format:
- 4 bytes per pixel (R, G, B, A)
- Buffer size = width × height × 4 bytes
- Row-major order, no padding
- Alpha channel always present (0xFF for opaque)

---

## 5. Render Loop and Buffer Management

### 5.1 Multi-Buffer Architecture

The system uses **triple buffering** for tear-free rendering:

```
LCD Hardware Buffers:
─────────────────────
  Buffer 0 ──┐
              ├─── s_lcd_buffers[3]
  Buffer 1 ──┤     (allocated in PSRAM)
              │
  Buffer 2 ──┘

Indices:
────────
  s_render_buffer_index    = next buffer to render to
  s_last_display_buffer    = currently displaying

Flow:
─────
  Frame N:   Render → Buffer 0    Display → Buffer 2
  Frame N+1: Render → Buffer 1    Display → Buffer 0
  Frame N+2: Render → Buffer 2    Display → Buffer 1
```

### 5.2 Render Task Loop

**Main Render Function** (`lcd_animation_task` in `animation_player_render.c`):

```
while (true) {
    1. Wait for VSync (if multi-buffering enabled)
       ↓
    2. Check render mode and acknowledge
       - s_render_mode_active = s_render_mode_request
       ↓
    3. Get back buffer (next render target)
       - back_buffer = s_lcd_buffers[s_render_buffer_index]
       ↓
    4. If UI mode:
       - Call ugfx_ui_render_to_buffer()
       - Skip to step 8
       ↓
    5. If Animation mode:
       a. Check for back buffer prefetch
          - If pending: call prefetch_first_frame()
       b. Check for buffer swap
          - If requested and ready: swap front ↔ back
       c. Render animation frame
          - Call render_next_frame(s_front_buffer)
       ↓
    6. Cache sync (if enabled)
       - esp_cache_msync() to flush to PSRAM
       ↓
    7. Flip buffers
       - s_last_display_buffer = s_render_buffer_index
       - s_render_buffer_index = (index + 1) % 3
       ↓
    8. Frame timing delay
       - Calculate residual time
       - vTaskDelay() if under budget
       ↓
    9. DMA transfer to LCD
       - esp_lcd_panel_draw_bitmap()
       ↓
    10. Update timing statistics
        - Frame duration tracking
}
```

### 5.3 Frame Rendering Pipeline

**render_next_frame()** implementation:

```c
int render_next_frame(animation_buffer_t *buf, 
                     uint8_t *dest_buffer,
                     int target_w, int target_h,
                     bool use_prefetched)
```

Steps:
```
1. If use_prefetched and first frame ready:
   - memcpy prefetched frame to dest_buffer
   - Return prefetched delay
   ↓
2. Else decode next frame:
   a. Select decode buffer (double-buffered)
   b. animation_decoder_decode_next()
   c. Get frame delay
   d. Swap decode buffers
   ↓
3. Upscale decoded frame to LCD resolution:
   a. Set upscale parameters (src, dst, lookup tables)
   b. Split work: top half and bottom half
   c. Notify worker tasks
   d. Wait for both workers to complete
   ↓
4. Return frame delay in milliseconds
```

### 5.4 Upscaling System

**Multi-threaded Upscaling**:

The system uses **2 worker tasks** for parallel upscaling:
- `upscale_worker_top`: Renders rows 0 to 360
- `upscale_worker_bottom`: Renders rows 360 to 720

**Algorithm**:
```c
void blit_upscaled_rows(src_rgba, src_w, src_h,
                        dst_buffer, dst_w, dst_h,
                        row_start, row_end,
                         lookup_x, lookup_y)
```

For each destination row:
1. Use `lookup_y[dst_y]` to get source row
2. For each destination pixel:
   - Use `lookup_x[dst_x]` to get source pixel
   - Convert RGBA → RGB565 or RGB888
   - Write to destination buffer

**Lookup Table Generation**:
```c
// Nearest-neighbor mapping
for (int i = 0; i < dst_dim; i++) {
    lookup[i] = (uint16_t)((i * src_dim) / dst_dim);
}
```

This enables O(1) per-pixel coordinate mapping with cache-friendly access.

### 5.5 VSync Synchronization

**VSync Callback**:
```c
bool lcd_panel_refresh_done_cb(esp_lcd_panel_handle_t panel,
                               esp_lcd_dpi_panel_event_data_t *edata,
                               void *user_ctx)
{
    // Called from ISR when DMA completes
    xSemaphoreGiveFromISR(s_vsync_sem, &higher_prio_woken);
}
```

**Render Task**:
```c
// Wait for LCD to finish displaying previous frame
xSemaphoreTake(s_vsync_sem, portMAX_DELAY);

// Now safe to render next frame
```

This ensures:
- No tearing (never write to displaying buffer)
- Smooth 60 FPS when animations support it
- Efficient CPU utilization (blocked while idle)

### 5.6 Prefetch Optimization

**Purpose**: Eliminate first-frame delay when swapping animations.

**Implementation**:
1. When back buffer loaded, set `prefetch_pending = true`
2. Render task detects flag and calls `prefetch_first_frame()`
3. Prefetch decodes and upscales first frame to `prefetched_first_frame` buffer
4. On swap, render task uses prefetched frame (instant display)
5. Subsequent frames decode normally

**Benefits**:
- Seamless transitions between animations
- No black frame when changing artwork
- First frame appears within 16ms (single frame time)

---

## 6. Threading Model

### 6.1 Task Overview

```
┌──────────────────────────────────────────────────────┐
│                     Core 0 (CPU0)                     │
│                                                       │
│  ┌───────────────────┐  ┌───────────────────┐       │
│  │ upscale_worker_   │  │ upscale_worker_   │       │
│  │      top          │  │     bottom        │       │
│  │ Priority: 20      │  │ Priority: 20      │       │
│  │ Stack: 2KB        │  │ Stack: 2KB        │       │
│  └───────────────────┘  └───────────────────┘       │
│                                                       │
│  ┌─────────────────────────────────────────┐        │
│  │         Touch Task (BSP)                 │        │
│  │         Priority: 10                     │        │
│  └─────────────────────────────────────────┘        │
│                                                       │
└──────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────┐
│                     Core 1 (CPU1)                     │
│                                                       │
│  ┌─────────────────────────────────────────┐        │
│  │      lcd_animation_task                  │        │
│  │      (Main Render Loop)                  │        │
│  │      Priority: 20                        │        │
│  │      Stack: 32KB                         │        │
│  └─────────────────────────────────────────┘        │
│                                                       │
│  ┌─────────────────────────────────────────┐        │
│  │      animation_loader_task               │        │
│  │      (Background Loading)                │        │
│  │      Priority: 5                         │        │
│  │      Stack: 8KB                          │        │
│  └─────────────────────────────────────────┘        │
│                                                       │
│  ┌─────────────────────────────────────────┐        │
│  │      HTTP Server Tasks                   │        │
│  │      WebSocket, REST API                 │        │
│  └─────────────────────────────────────────┘        │
│                                                       │
└──────────────────────────────────────────────────────┘
```

### 6.2 Task Priorities

From `sdkconfig`:
```
CONFIG_P3A_RENDER_TASK_PRIORITY=20
CONFIG_P3A_LOADER_TASK_PRIORITY=5
```

Priority hierarchy:
1. **Priority 20**: Render task + upscale workers (time-critical)
2. **Priority 10**: Touch input (responsive UI)
3. **Priority 5**: Background loader (low priority, non-blocking)
4. **Priority 5**: Network tasks (HTTP, WebSocket)

### 6.3 Synchronization Primitives

**Mutexes**:
```c
SemaphoreHandle_t s_buffer_mutex;  // Protects animation_buffer_t state
```
- Held when reading/writing buffer metadata
- NOT held during decode or render (long operations)
- Prevents race conditions on swap operations

**Binary Semaphores**:
```c
SemaphoreHandle_t s_vsync_sem;     // VSync notification
SemaphoreHandle_t s_loader_sem;    // Loader wake-up signal
```
- VSync: ISR → Render task signaling
- Loader: Control API → Loader task signaling

**Task Notifications**:
```c
xTaskNotify(s_upscale_worker_top, 1, eSetBits);
xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);
```
- Used for upscale worker coordination
- More efficient than semaphores for task-to-task signaling

### 6.4 Memory Barriers

```c
#if defined(__XTENSA__)
#define MEMORY_BARRIER() xthal_dcache_sync()
#elif defined(__riscv)
#define MEMORY_BARRIER() __asm__ __volatile__ ("fence" ::: "memory")
#else
#define MEMORY_BARRIER() __asm__ __volatile__ ("" ::: "memory")
#endif
```

Memory barriers ensure:
- Writes from render task visible to upscale workers
- Upscale worker completion visible to render task
- Critical for multi-core correctness

Placement:
- Before notifying workers (after setting upscale params)
- After workers complete (before reading result)

### 6.5 Concurrency Patterns

**Producer-Consumer** (Loader → Render):
```
Loader Task:              Render Task:
───────────               ────────────
Load animation            
  ↓
Lock mutex
Set back_buffer.ready = true
Set swap_requested = true
Unlock mutex
                         ───────────────→
                          Lock mutex
                          Check swap_requested
                          Swap buffers
                          Unlock mutex
                          Render from new front
```

**Work Distribution** (Render → Upscale Workers):
```
Render Task:              Worker Top:         Worker Bottom:
────────────              ───────────         ──────────────
Set upscale params
  ↓
MEMORY_BARRIER()
  ↓
Notify top worker    →   Wakeup
Notify bottom worker →                       Wakeup
  ↓                       Blit rows 0-360     Blit rows 360-720
Wait for both              ↓                   ↓
  ↓                       BARRIER()            BARRIER()
  ↓                       Notify done          Notify done
Check completion     ←    ───────────────────────────────────
  ↓
MEMORY_BARRIER()
  ↓
Use rendered frame
```

---

## 7. Data Flow Diagrams

### 7.1 Startup Sequence

```
┌─────────────┐
│  app_main() │
└──────┬──────┘
       │
       ├─→ NVS init
       ├─→ Network init
       ├─→ LCD init (app_lcd_p4.c)
       │   ├─→ Allocate 3 LCD buffers (PSRAM)
       │   └─→ Initialize DPI panel
       │
       ├─→ animation_player_init()
       │   ├─→ Channel system init
       │   │   ├─→ sdcard_channel_init()
       │   │   ├─→ Mount SD card
       │   │   ├─→ sdcard_channel_refresh("/sdcard/animations")
       │   │   ├─→ channel_player_init()
       │   │   └─→ channel_player_load_channel()
       │   │
       │   ├─→ Load first animation
       │   │   ├─→ channel_player_get_current_post()
       │   │   └─→ load_animation_into_buffer()
       │   │       ├─→ Read file to SPIRAM
       │   │       ├─→ animation_decoder_init()
       │   │       ├─→ Allocate RGBA buffers
       │   │       └─→ Generate upscale lookups
       │   │
       │   ├─→ Create upscale workers (core 0)
       │   └─→ Create loader task (core 1)
       │
       ├─→ animation_player_start()
       │   └─→ Create render task (core 1)
       │       └─→ lcd_animation_task()
       │
       ├─→ Touch init (app_touch.c)
       ├─→ USB init (app_usb.c)
       └─→ HTTP server init
```

### 7.2 Frame Render Sequence

```
┌──────────────────────────────────────────────────┐
│             lcd_animation_task()                  │
│              (Render Loop - Core 1)               │
└────────────────────┬─────────────────────────────┘
                     │
         ┌───────────▼──────────┐
         │ Wait for VSync       │
         └───────────┬──────────┘
                     │
         ┌───────────▼──────────┐
         │ Get back buffer      │
         │ (next render target) │
         └───────────┬──────────┘
                     │
         ┌───────────▼──────────┐
         │ Check render mode    │
         └─────┬──────────┬─────┘
               │          │
       UI mode │          │ Animation mode
               │          │
    ┌──────────▼─┐   ┌────▼──────────────────────┐
    │ Render UI  │   │ Check swap/prefetch state │
    └──────────┬─┘   └────┬──────────────────────┘
               │          │
               │     ┌────▼────────────────┐
               │     │ Handle prefetch?    │
               │     └────┬───────────┬────┘
               │          No          Yes
               │          │           │
               │          │      ┌────▼─────────────┐
               │          │      │ prefetch_first_  │
               │          │      │    frame()       │
               │          │      └────┬─────────────┘
               │          │           │
               │     ┌────▼───────────▼────┐
               │     │ Ready to swap?      │
               │     └────┬───────────┬────┘
               │          No          Yes
               │          │           │
               │          │      ┌────▼─────────────┐
               │          │      │ Swap front ↔ back│
               │          │      └────┬─────────────┘
               │          │           │
               │     ┌────▼───────────▼────┐
               │     │ render_next_frame() │
               │     │                     │
               │     │ 1. Decode RGBA      │◀───┐
               │     │ 2. Start workers    │    │
               │     │ 3. Wait completion  │    │
               │     └────┬────────────────┘    │
               │          │                     │
               └──────────┼─────────────────────┘
                          │
              ┌───────────▼──────────┐
              │ Cache sync (flush)   │
              └───────────┬──────────┘
                          │
              ┌───────────▼──────────┐
              │ Flip buffer indices  │
              └───────────┬──────────┘
                          │
              ┌───────────▼──────────┐
              │ Frame timing delay   │
              └───────────┬──────────┘
                          │
              ┌───────────▼──────────┐
              │ DMA to LCD           │
              │ (triggers VSync)     │
              └───────────┬──────────┘
                          │
                        Loop back
```

### 7.3 Animation Cycling Flow

```
User Action (Touch/Web/MQTT):
"Next Animation"
     │
     ▼
animation_player_cycle_animation(forward=true)
     │
     ├─→ channel_player_advance()
     │   ├─→ current_pos++
     │   └─→ If at end: wrap, optionally re-shuffle
     │
     ├─→ Lock mutex
     ├─→ s_swap_requested = true
     ├─→ Unlock mutex
     │
     └─→ xSemaphoreGive(s_loader_sem)
              │
              ▼
     animation_loader_task() wakes up
              │
              ├─→ channel_player_get_current_post()
              │   └─→ Returns &posts[indices[current_pos]]
              │
              ├─→ load_animation_into_buffer()
              │   ├─→ Read file
              │   ├─→ Init decoder
              │   ├─→ Allocate buffers
              │   └─→ Generate lookups
              │
              ├─→ Lock mutex
              ├─→ back_buffer.prefetch_pending = true
              ├─→ Unlock mutex
              │
              ▼
     Render task (next iteration):
              │
              ├─→ Detects prefetch_pending
              ├─→ Calls prefetch_first_frame()
              │   └─→ Decode + upscale first frame
              │
              ├─→ back_buffer.ready = true
              │
              ▼
     Render task (next iteration):
              │
              ├─→ Detects swap_requested && ready
              ├─→ Swaps front ↔ back buffers
              │
              └─→ Starts rendering new animation
                  (using prefetched first frame)
```

---

## 8. Key API Reference

### 8.1 Public Animation Player API

**Initialization**:
```c
esp_err_t animation_player_init(
    esp_lcd_panel_handle_t display_handle,  // LCD panel handle
    uint8_t **lcd_buffers,                  // Array of LCD buffers
    uint8_t buffer_count,                   // Number of buffers (3)
    size_t buffer_bytes,                    // Size per buffer
    size_t row_stride_bytes                 // Row stride
);

esp_err_t animation_player_start(void);
void animation_player_deinit(void);
```

**Playback Control**:
```c
void animation_player_set_paused(bool paused);
void animation_player_toggle_pause(void);
bool animation_player_is_paused(void);

void animation_player_cycle_animation(bool forward);
// forward=true: next, forward=false: previous
```

**Mode Switching**:
```c
esp_err_t animation_player_enter_ui_mode(void);
// Blocks until render loop acknowledges

void animation_player_exit_ui_mode(void);
// Blocks until render loop acknowledges

bool animation_player_is_ui_mode(void);
```

**Direct Control**:
```c
size_t animation_player_get_current_index(void);

esp_err_t animation_player_swap_to_index(size_t index);
// Load specific post by index

esp_err_t animation_player_add_file(
    const char *filename,
    const char *animations_dir,
    size_t insert_after_index,
    size_t *out_index
);
```

### 8.2 Channel System API

**SD Card Channel**:
```c
esp_err_t sdcard_channel_init(void);
void sdcard_channel_deinit(void);

esp_err_t sdcard_channel_refresh(const char *animations_dir);

esp_err_t sdcard_channel_query(
    const sdcard_query_t *query,
    sdcard_query_result_t *result
);
// Query structure:
typedef struct {
    size_t offset;
    size_t count;
    sdcard_sort_order_t sort_order;  // SDCARD_SORT_BY_NAME or _DATE
} sdcard_query_t;

size_t sdcard_channel_get_total_count(void);

void sdcard_channel_mark_unhealthy(size_t post_index);
```

**Channel Player**:
```c
esp_err_t channel_player_init(void);
void channel_player_deinit(void);

esp_err_t channel_player_load_channel(void);

esp_err_t channel_player_get_current_post(
    const sdcard_post_t **out_post
);

esp_err_t channel_player_advance(void);
esp_err_t channel_player_go_back(void);

void channel_player_set_randomize(bool enable);
bool channel_player_is_randomized(void);

size_t channel_player_get_current_position(void);
size_t channel_player_get_post_count(void);
```

### 8.3 Decoder API

```c
esp_err_t animation_decoder_init(
    animation_decoder_t **decoder,
    animation_decoder_type_t type,
    const uint8_t *data,
    size_t size
);

esp_err_t animation_decoder_get_info(
    animation_decoder_t *decoder,
    animation_decoder_info_t *info
);

esp_err_t animation_decoder_decode_next(
    animation_decoder_t *decoder,
    uint8_t *rgba_buffer  // Must be width × height × 4 bytes
);

esp_err_t animation_decoder_get_frame_delay(
    animation_decoder_t *decoder,
    uint32_t *delay_ms
);

esp_err_t animation_decoder_reset(animation_decoder_t *decoder);

void animation_decoder_unload(animation_decoder_t **decoder);
```

---

## 9. Extension Points

### 9.1 Adding New Content Sources

**Current**: SD card only
**Goal**: Support network sources (cloud storage, HTTP streaming, MQTT)

**Approach**:
1. Define channel interface abstraction:
```c
typedef struct channel_ops_s {
    esp_err_t (*init)(void);
    esp_err_t (*refresh)(const char *source);
    esp_err_t (*query)(const sdcard_query_t *q, 
                       sdcard_query_result_t *r);
    esp_err_t (*get_post)(size_t idx, const sdcard_post_t **post);
    void (*mark_unhealthy)(size_t idx);
    void (*deinit)(void);
} channel_ops_t;
```

2. Implement for new sources:
```c
// cloud_channel.c
static esp_err_t cloud_channel_refresh(const char *source) {
    // HTTP GET list of artworks from API
    // Parse JSON response
    // Populate posts array
}

channel_ops_t cloud_channel_ops = {
    .init = cloud_channel_init,
    .refresh = cloud_channel_refresh,
    .query = cloud_channel_query,
    // ...
};
```

3. Update channel player to use ops table:
```c
typedef struct {
    channel_ops_t *ops;
    void *context;
} channel_t;

esp_err_t channel_player_use_channel(channel_t *channel);
```

### 9.2 Adding New Decoder Formats

**Current**: WebP, GIF, PNG, JPEG
**Goal**: Support APNG, AVIF, WebM, etc.

**Steps**:
1. Add new type to enum:
```c
typedef enum {
    ANIMATION_DECODER_TYPE_WEBP,
    ANIMATION_DECODER_TYPE_GIF,
    ANIMATION_DECODER_TYPE_PNG,
    ANIMATION_DECODER_TYPE_JPEG,
    ANIMATION_DECODER_TYPE_APNG,  // New
} animation_decoder_type_t;
```

2. Extend internal union:
```c
struct animation_decoder_s {
    animation_decoder_type_t type;
    union {
        struct { /*...*/ } webp;
        struct { /*...*/ } gif;
        struct { /*...*/ } png;
        struct { /*...*/ } jpeg;
        struct {
            void *apng_decoder;
            // APNG-specific state
        } apng;  // New
    } impl;
};
```

3. Implement decoder functions in new file `apng_animation_decoder.c`:
```c
esp_err_t apng_decoder_init(/*...*/);
esp_err_t apng_decoder_decode_next(/*...*/);
// ...
```

4. Update `animation_decoder_init()` switch statement
5. Update `sdcard_channel.c` to recognize `.apng` extension

### 9.3 Custom Post Metadata

**Current**: name, created_at, filepath, type, healthy
**Goal**: Add artist, tags, favorites, view count, etc.

**Approach**:
1. Extend `sdcard_post_t`:
```c
typedef struct {
    char *name;
    time_t created_at;
    char *filepath;
    asset_type_t type;
    bool healthy;
    
    // New fields
    char *artist;
    char **tags;
    size_t tag_count;
    bool favorite;
    uint32_t view_count;
} sdcard_post_t;
```

2. Update `sdcard_channel_refresh()` to read metadata:
```c
// Option 1: Sidecar JSON files
// /sdcard/animations/artwork.webp
// /sdcard/animations/artwork.json  <- metadata

// Option 2: Embedded metadata (EXIF, XMP)
// Parse from file header

// Option 3: Central database
// /sdcard/animations/.metadata.db
```

3. Add filtering to `sdcard_query_t`:
```c
typedef struct {
    size_t offset;
    size_t count;
    sdcard_sort_order_t sort_order;
    
    // New filters
    const char *artist_filter;
    const char **tag_filter;
    size_t tag_filter_count;
    bool favorites_only;
} sdcard_query_t;
```

4. Update `sdcard_channel_query()` to apply filters

### 9.4 Advanced Playback Modes

**Playlist Support**:
```c
typedef struct {
    char *name;
    size_t *post_indices;
    size_t count;
} playlist_t;

esp_err_t channel_player_load_playlist(playlist_t *playlist);
```

**Smart Shuffle** (avoid repeats):
```c
// Track recently played posts
#define RECENT_HISTORY_SIZE 50
size_t recent_history[RECENT_HISTORY_SIZE];

// Shuffle with history exclusion
void smart_shuffle_indices(size_t *indices, size_t count,
                          size_t *history, size_t history_size);
```

**Scheduled Playback**:
```c
typedef struct {
    time_t start_time;
    time_t end_time;
    size_t post_index;
} scheduled_post_t;

// Check schedule before advancing
if (is_scheduled_time()) {
    post = get_scheduled_post();
} else {
    post = channel_player_get_current_post();
}
```

### 9.5 Performance Optimizations

**Decode Caching**:
```c
typedef struct {
    size_t post_index;
    uint8_t **decoded_frames;
    uint32_t *frame_delays;
    size_t frame_count;
} decoded_cache_entry_t;

// Keep last N animations fully decoded in PSRAM
decoded_cache_entry_t cache[CACHE_SIZE];
```

**Predictive Prefetch**:
```c
// Load next N animations in background
void prefetch_upcoming_posts(size_t count) {
    for (size_t i = 1; i <= count; i++) {
        size_t next_pos = (current_pos + i) % post_count;
        start_background_load(next_pos);
    }
}
```

**Hardware Acceleration**:
```c
// Use ESP32-P4's 2D graphics accelerator (PPA)
esp_err_t ppa_upscale(const uint8_t *src, uint8_t *dst,
                     int src_w, int src_h,
                     int dst_w, int dst_h);
```

---

## Conclusion

The p3a animation pipeline is a sophisticated, multi-layered system designed for smooth, tear-free playback of pixel art animations. Key architectural strengths include:

1. **Clean Abstraction**: Channel system decouples content storage from playback logic
2. **High Performance**: Multi-threaded rendering, hardware DMA, triple buffering
3. **Flexibility**: Pluggable decoders, extensible channel system
4. **Robustness**: Error handling, health tracking, graceful degradation
5. **Efficiency**: Prefetching, double buffering, memory-mapped files

The channels abstraction provides a solid foundation for future enhancements like cloud synchronization, curated feeds, and social features while maintaining backward compatibility with local SD card storage.

---

## Appendix: File Reference

**Core Animation Files**:
- `main/animation_player.c` (641 lines) - Main orchestrator
- `main/animation_player_render.c` (678 lines) - Render loop
- `main/animation_player_loader.c` (502 lines) - Background loading
- `main/animation_player_priv.h` (191 lines) - Internal structures

**Channel System**:
- `main/sdcard_channel.c` (380 lines) - SD card abstraction
- `main/channel_player.c` (283 lines) - Playback sequencing
- `main/include/sdcard_channel.h` (136 lines) - Channel interface
- `main/include/channel_player.h` (106 lines) - Player interface

**Decoder System**:
- `main/include/animation_decoder.h` (96 lines) - Public API
- `main/include/animation_decoder_internal.h` (40 lines) - Internal structure
- Format-specific decoders in `main/` and `components/`

**Display System**:
- `main/app_lcd_p4.c` - LCD initialization and buffer allocation
- `main/ugfx_ui.c` - UI rendering for menus

---

*Document Version: 1.0*  
*Last Updated: 2025-12-04*  
*Author: Technical Documentation System*
