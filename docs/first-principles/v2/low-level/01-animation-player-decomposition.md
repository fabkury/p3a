# Animation Player Decomposition

> **Extends**: v1/low-level/make-animation-player-state-private.md, v1/low-level/encapsulate-display-renderer-globals.md  
> **Phase**: 3 (Domain Refinement)

## Goal

Decompose the 800+ line `animation_player.c` into focused modules with single responsibilities, reducing coupling and improving testability.

## Current State (v2 Assessment)

`main/animation_player.c` handles too many concerns:

| Lines | Responsibility |
|-------|---------------|
| 1-50 | Includes (24 headers!) |
| 50-150 | Global state variables |
| 150-300 | SD card mounting and discovery |
| 300-500 | Render dispatch and UI mode |
| 500-700 | Swap request handling |
| 700-880 | Loader task and initialization |

### Current Global State

```c
// From animation_player.c
animation_buffer_t s_front_buffer = {0};
animation_buffer_t s_back_buffer = {0};
size_t s_next_asset_index = 0;
bool s_swap_requested = false;
bool s_loader_busy = false;
volatile bool s_cycle_pending = false;
volatile bool s_cycle_forward = true;
TaskHandle_t s_loader_task = NULL;
SemaphoreHandle_t s_loader_sem = NULL;
SemaphoreHandle_t s_buffer_mutex = NULL;
SemaphoreHandle_t s_prefetch_done_sem = NULL;
bool s_anim_paused = false;
animation_load_override_t s_load_override = {0};
app_lcd_sd_file_list_t s_sd_file_list = {0};
bool s_sd_mounted = false;
bool s_sd_export_active = false;
bool s_sd_access_paused = false;
```

**17 global variables** with unclear ownership and lifetime.

## v1 Alignment

v1 identifies the need to privatize state and encapsulate globals. v2 provides the decomposition strategy.

## Proposed Module Split

```
animation_player.c (800+ lines)
            │
            ▼
┌───────────────────────────────────────────────────────────┐
│                                                           │
│  ┌─────────────────┐  ┌─────────────────┐                │
│  │ frame_buffers.c │  │ loader_task.c   │                │
│  │                 │  │                 │                │
│  │ - front/back    │  │ - SD I/O        │                │
│  │ - swap logic    │  │ - decode init   │                │
│  │ - mutex         │  │ - prefetch      │                │
│  │ ~150 lines      │  │ ~200 lines      │                │
│  └─────────────────┘  └─────────────────┘                │
│                                                           │
│  ┌─────────────────┐  ┌─────────────────┐                │
│  │ render_loop.c   │  │ sd_discovery.c  │                │
│  │                 │  │                 │                │
│  │ - dispatch      │  │ - file list     │                │
│  │ - UI mode       │  │ - mount/unmount │                │
│  │ - timing        │  │ - refresh       │                │
│  │ ~150 lines      │  │ ~150 lines      │                │
│  └─────────────────┘  └─────────────────┘                │
│                                                           │
│  ┌─────────────────────────────────────────────────────┐ │
│  │ animation_player.c (facade)                         │ │
│  │                                                     │ │
│  │ - Public API implementation                         │ │
│  │ - Delegates to modules above                        │ │
│  │ - ~150 lines                                        │ │
│  └─────────────────────────────────────────────────────┘ │
│                                                           │
└───────────────────────────────────────────────────────────┘
```

## Module Definitions

### 1. Frame Buffers Module

```c
// frame_buffers.h (internal)
typedef struct {
    uint8_t* data;
    size_t size;
    void* decoder_handle;
    // ... metadata
} animation_buffer_t;

typedef struct {
    animation_buffer_t front;
    animation_buffer_t back;
    SemaphoreHandle_t mutex;
    bool swap_pending;
} frame_buffer_state_t;

esp_err_t frame_buffers_init(size_t buffer_size);
void frame_buffers_deinit(void);
animation_buffer_t* frame_buffers_get_front(void);
animation_buffer_t* frame_buffers_get_back(void);
esp_err_t frame_buffers_swap(void);
bool frame_buffers_is_swap_pending(void);
```

### 2. Loader Task Module

```c
// loader_task.h (internal)
typedef struct {
    char filepath[256];
    bool override_active;
    // ... load parameters
} load_request_t;

esp_err_t loader_task_init(void);
void loader_task_deinit(void);
esp_err_t loader_task_request_load(const load_request_t* req);
bool loader_task_is_busy(void);
void loader_task_wait_idle(void);
void loader_task_pause_sd(void);
void loader_task_resume_sd(void);
```

### 3. Render Loop Module

```c
// render_loop.h (internal)
typedef int (*render_callback_t)(uint8_t* buffer, void* ctx);

esp_err_t render_loop_init(void);
void render_loop_deinit(void);
esp_err_t render_loop_start(void);
esp_err_t render_loop_enter_ui_mode(void);
void render_loop_exit_ui_mode(void);
bool render_loop_is_ui_mode(void);
void render_loop_set_callback(render_callback_t cb, void* ctx);
```

### 4. SD Discovery Module

```c
// sd_discovery.h (internal)
typedef struct {
    char** files;
    size_t count;
    size_t capacity;
} file_list_t;

esp_err_t sd_discovery_init(void);
void sd_discovery_deinit(void);
esp_err_t sd_discovery_mount(void);
esp_err_t sd_discovery_unmount(void);
bool sd_discovery_is_mounted(void);
esp_err_t sd_discovery_refresh(file_list_t* out);
esp_err_t sd_discovery_get_animations_dir(char* out, size_t max_len);
```

### 5. Animation Player Facade

```c
// animation_player.c (reduced)
#include "frame_buffers.h"
#include "loader_task.h"
#include "render_loop.h"
#include "sd_discovery.h"

esp_err_t animation_player_init(...) {
    frame_buffers_init(buffer_size);
    loader_task_init();
    render_loop_init();
    sd_discovery_init();
    return ESP_OK;
}

esp_err_t animation_player_request_swap(const swap_request_t* req) {
    load_request_t load_req = {
        .filepath = req->filepath,
        // ...
    };
    return loader_task_request_load(&load_req);
}

// ... other public API delegates
```

## Migration Steps

### Step 1: Create State Struct

First, group globals without changing behavior:

```c
// Before
static animation_buffer_t s_front_buffer;
static bool s_swap_requested;
// ... 15 more

// After
typedef struct {
    animation_buffer_t front_buffer;
    animation_buffer_t back_buffer;
    bool swap_requested;
    // ... all state
} animation_player_state_t;

static animation_player_state_t s_state = {0};
```

### Step 2: Extract Frame Buffers

Move buffer-related code to `frame_buffers.c`:
- `s_front_buffer`, `s_back_buffer`
- `s_buffer_mutex`
- `s_swap_requested`
- Swap logic functions

### Step 3: Extract Loader Task

Move loader-related code to `loader_task.c`:
- `s_loader_task`, `s_loader_sem`
- `s_loader_busy`
- `s_sd_access_paused`
- Task function and request handling

### Step 4: Extract SD Discovery

Move SD-related code to `sd_discovery.c`:
- `s_sd_mounted`, `s_sd_file_list`
- `mount_sd_and_discover()`
- `refresh_animation_file_list()`

### Step 5: Extract Render Loop

Move render-related code to `render_loop.c`:
- `animation_player_render_dispatch_cb()`
- UI mode logic
- Callback management

### Step 6: Reduce Facade

Final `animation_player.c` delegates to modules.

## Header Reduction

After decomposition, `animation_player.c` includes:

```c
// Before: 24 includes
// After: ~8 includes
#include "animation_player_priv.h"  // Private header with state
#include "frame_buffers.h"
#include "loader_task.h"
#include "render_loop.h"
#include "sd_discovery.h"
#include "play_scheduler.h"         // For swap coordination
#include "p3a_state.h"              // For state queries
#include "config_store.h"           // For settings
```

## Success Criteria

- [ ] `animation_player.c` < 300 lines
- [ ] Each module < 200 lines
- [ ] No module has > 8 includes
- [ ] All globals are static within their module
- [ ] Modules can be unit tested in isolation

## Risks

| Risk | Mitigation |
|------|------------|
| Breaking render timing | Comprehensive testing, A/B comparison |
| Increased call overhead | Inline critical paths |
| State synchronization bugs | Clear ownership, mutex usage |
