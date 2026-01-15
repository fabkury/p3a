# Content Pipeline Refactoring

> **New in v2**  
> **Phase**: 3 (Domain Refinement)

## Goal

Establish a clear, linear content pipeline: **Source → Cache → Queue → Decode → Render → Display**. Each stage has a single responsibility and explicit interfaces.

## Current State (v2 Assessment)

The current content flow is interleaved across multiple files:

| Stage | Current Location | Problem |
|-------|-----------------|---------|
| Source selection | `channel_manager/`, `play_scheduler.c` | Split across 15+ files |
| Caching | `vault_storage.c`, `download_manager.c` | Overlapping responsibilities |
| Queuing | `play_scheduler.c`, `animation_player.c` | Unclear ownership |
| Decoding | `animation_decoder/` | Clean, but tightly coupled to player |
| Rendering | `animation_player_render.c`, `display_upscaler.c` | Mixed with loading logic |
| Display | `display_renderer.c` | Good separation |

`animation_player.c` (800+ lines) handles SD mounting, file discovery, buffer management, swap coordination, and UI mode transitions.

## Proposed Pipeline Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         CONTENT PIPELINE                                │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐             │
│  │   CONTENT    │───▶│   CONTENT    │───▶│   PLAYBACK   │             │
│  │   SOURCE     │    │    CACHE     │    │    QUEUE     │             │
│  │              │    │              │    │              │             │
│  │ - SD card    │    │ - Vault      │    │ - History    │             │
│  │ - Makapix    │    │ - Prefetch   │    │ - Lookahead  │             │
│  │ - Artwork    │    │ - LRU evict  │    │ - SWRR       │             │
│  └──────────────┘    └──────────────┘    └──────────────┘             │
│         │                   │                   │                      │
│         │ channel_post_t    │ cached_item_t     │ swap_request_t      │
│         ▼                   ▼                   ▼                      │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐             │
│  │   DECODER    │◀───│   LOADER     │◀───│   RENDER     │             │
│  │   POOL       │    │   SERVICE    │    │   ENGINE     │             │
│  │              │    │              │    │              │             │
│  │ - WebP       │    │ - File I/O   │    │ - Upscale    │             │
│  │ - GIF        │    │ - Memory mgmt│    │ - Alpha      │             │
│  │ - PNG/JPEG   │    │ - Error retry│    │ - Rotation   │             │
│  └──────────────┘    └──────────────┘    └──────────────┘             │
│         │                   │                   │                      │
│         │ frame_t           │ animation_t       │ framebuffer_t       │
│         ▼                   ▼                   ▼                      │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │                      DISPLAY ENGINE                              │  │
│  │                                                                  │  │
│  │  - Frame buffer management (triple buffering)                   │  │
│  │  - VSYNC synchronization                                        │  │
│  │  - DMA transfer to LCD                                          │  │
│  └─────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

## Component Definitions

### Content Source (existing, refactored)

```c
// content_source.h - unified interface for artwork sources
typedef struct {
    // Source identity
    const char* source_id;          // "sdcard", "makapix:all", "makapix:promoted"
    
    // Operations (vtable pattern from channel_interface.h)
    esp_err_t (*load)(content_source_t* src);
    esp_err_t (*get_post)(content_source_t* src, size_t index, channel_post_t* out);
    size_t (*get_count)(content_source_t* src);
    esp_err_t (*refresh)(content_source_t* src);
    void (*destroy)(content_source_t* src);
} content_source_t;
```

### Content Cache (consolidate vault + download)

```c
// content_cache.h - unified caching layer
typedef struct {
    char storage_key[64];
    char filepath[256];
    bool is_cached;
    bool is_downloading;
    int download_progress;          // 0-100
} cached_item_t;

esp_err_t content_cache_init(void);
esp_err_t content_cache_get(const char* storage_key, cached_item_t* out);
esp_err_t content_cache_prefetch(const char* storage_key, const char* url);
esp_err_t content_cache_evict_lru(size_t bytes_needed);
```

### Playback Queue (existing play_scheduler, refined)

```c
// playback_queue.h - what to play next
typedef struct {
    swap_request_t request;
    bool is_ready;                  // Artwork is cached and decodable
} queued_item_t;

esp_err_t playback_queue_next(queued_item_t* out);
esp_err_t playback_queue_prev(queued_item_t* out);
esp_err_t playback_queue_peek(queued_item_t* out);
```

### Loader Service (new, extracted from animation_player)

```c
// loader_service.h - file I/O and memory management
typedef struct {
    void* decoder_handle;
    size_t frame_count;
    int frame_delay_ms;
    int width, height;
    bool has_alpha;
} loaded_animation_t;

esp_err_t loader_service_init(void);
esp_err_t loader_service_load(const char* filepath, loaded_animation_t* out);
void loader_service_unload(loaded_animation_t* anim);
bool loader_service_is_busy(void);
```

### Render Engine (existing, clarified interface)

```c
// render_engine.h - frame processing
typedef struct {
    uint8_t* pixels;                // RGBA or RGB
    int width, height;
    int delay_ms;
} raw_frame_t;

typedef struct {
    uint8_t* buffer;                // Display format (RGB888)
    size_t stride;
} output_frame_t;

esp_err_t render_engine_process_frame(const raw_frame_t* in, output_frame_t* out);
esp_err_t render_engine_set_rotation(display_rotation_t rotation);
esp_err_t render_engine_set_background(uint8_t r, uint8_t g, uint8_t b);
```

## Data Flow Types

```c
// Clear type progression through pipeline
typedef channel_post_t source_item_t;       // From content source
typedef cached_item_t cache_item_t;         // From cache layer
typedef swap_request_t queue_item_t;        // From playback queue
typedef loaded_animation_t animation_t;     // From loader
typedef raw_frame_t decoded_frame_t;        // From decoder
typedef output_frame_t display_frame_t;     // To display engine
```

## Migration Strategy

### Phase 1: Define Interfaces

Create header files without changing implementation:
- `content_source.h` (wraps `channel_interface.h`)
- `content_cache.h` (wraps `vault_storage.h`, `download_manager.h`)
- `loader_service.h` (wraps file loading from `animation_player.c`)

### Phase 2: Extract Loader Service

Move from `animation_player.c`:
- File reading logic → `loader_service.c`
- Decoder integration → `loader_service.c`
- Buffer management → stays (frame buffers are display concern)

### Phase 3: Consolidate Cache

Merge:
- `vault_storage.c` → `content_cache.c`
- Download logic from `download_manager.c` → `content_cache.c`
- Prefetch logic from `play_scheduler_cache.c` → `content_cache.c`

### Phase 4: Clean Animation Player

After extraction, `animation_player.c` becomes:
- Frame buffer coordination
- Render loop orchestration
- UI mode switching

Target: < 300 lines, single responsibility.

## Success Criteria

- [ ] Each pipeline stage has exactly one C file
- [ ] Data types clearly show progression (source → cache → queue → decode → render → display)
- [ ] Adding new content source requires only implementing `content_source_t`
- [ ] `animation_player.c` reduced to < 300 lines
- [ ] Prefetch and eviction logic centralized in `content_cache.c`

## Risks

| Risk | Mitigation |
|------|------------|
| Breaking existing playback | Feature flag for old vs new pipeline |
| Performance regression | Benchmark before/after each phase |
| Memory fragmentation | Clear ownership, explicit free points |
