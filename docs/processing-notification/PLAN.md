# Processing Notification Implementation Plan

## Overview

This document outlines the implementation plan for **processing-notification**, a visual feedback mechanism that provides immediate acknowledgment to users when p3a receives a user-initiated animation swap command.

## Implementation Progress

- [x] Plan document created
- [x] Plan updated with new requirements (triangle shape, NVS settings, failure handling)
- [ ] Step 1: Add config_store functions for notification settings
- [ ] Step 2: Add processing-notification state variables
- [ ] Step 3: Create display_processing_notification.c with triangle rendering
- [ ] Step 4: Integrate into render loop
- [ ] Step 5: Set flags at user-initiated entry points
- [ ] Step 6: Clear flags on successful swap
- [ ] Step 7: Handle failed swaps (red indicator for 3 seconds)
- [ ] Step 8: Update CMakeLists.txt
- [ ] Testing and verification

## Feature Definition

### What is Processing-Notification?

A **45-45-90 isosceles right triangle** drawn in the **bottom-right corner** of the display using a **checkerboard pattern** (every other pixel), providing immediate visual feedback that a user-initiated swap command has been received and is being processed.

### Visual Specification

- **Shape**: 45-45-90 isosceles right triangle with the right angle in the bottom-right corner
  - The hypotenuse runs from top-left to bottom-left to bottom-right
  - For size N, the triangle fills pixels where `(N - 1 - lx) <= ly` (where `lx, ly ∈ [0, N-1]`)
- **Size**: Configurable via NVS setting (default: 32, min: 8, max: 128)
- **Position**: Bottom-right corner, perfectly aligned
- **Color**: 
  - **Blue** (`0x0000FF`) during normal processing
  - **Red** (`0xFF0000`) on swap failure (displayed for 3 seconds)
- **Pattern**: Checkerboard pattern using local coordinates — for each pixel at local position `(lx, ly)`, the pixel is drawn if `(lx + ly) % 2 == 0`, otherwise skipped
- **Toggle**: Can be enabled/disabled via NVS setting (default: enabled)
- **Purpose**: The checkerboard pattern ensures the notification is visible regardless of background content

### NVS Settings

| Setting | JSON Key | Type | Default | Min | Max | Description |
|---------|----------|------|---------|-----|-----|-------------|
| Enabled | `proc_notif_enabled` | bool | true | - | - | Enable/disable processing notification |
| Size | `proc_notif_size` | uint8 | 32 | 8 | 128 | Triangle size in pixels |

### Lifecycle

```
┌─────────────────────────────────────────────────────────────────────────┐
│ User issues swap command (touchscreen tap, MQTT command, REST API)      │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ processing-notification state is SET to PROCESSING (blue)                │
│ (immediately, in the same context as receiving the command)              │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ Next render frame draws the blue checkerboard triangle                   │
│ (display_render_task checks state and overlays if active)                │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ Animation loader fetches/decodes new artwork                             │
│ (processing-notification continues to display on each frame)             │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                          ┌─────────┴─────────┐
                          │                   │
                     SUCCESS               FAILURE/TIMEOUT
                          │                   │
                          ▼                   ▼
┌────────────────────────────────┐  ┌─────────────────────────────────────┐
│ Buffer swap occurs             │  │ Swap fails or times out (5 seconds) │
│ State is CLEARED               │  │ State changes to FAILED (red)       │
└────────────────────────────────┘  └─────────────────────────────────────┘
                          │                   │
                          ▼                   ▼
┌────────────────────────────────┐  ┌─────────────────────────────────────┐
│ First frame of new animation   │  │ Red triangle displays for 3 seconds │
│ renders WITHOUT notification   │  │ Then state is CLEARED               │
└────────────────────────────────┘  └─────────────────────────────────────┘
```

## Architecture Analysis

### Swap Command Entry Points

User-initiated swap commands can originate from three sources:

1. **Touch gestures** (`main/app_touch.c`)
   - `P3A_TOUCH_EVENT_TAP_LEFT` → `app_lcd_cycle_animation_backward()` → `animation_player_cycle_animation(false)`
   - `P3A_TOUCH_EVENT_TAP_RIGHT` → `app_lcd_cycle_animation()` → `animation_player_cycle_animation(true)`

2. **REST API** (`components/http_api/http_api_rest.c`)
   - `POST /action/swap_next` → `api_enqueue_swap_next()` → queued command
   - `POST /action/swap_back` → `api_enqueue_swap_back()` → queued command
   - HTTP command queue is processed by `http_api_task` which calls `play_scheduler_next/prev`

3. **MQTT/Makapix** (`components/http_api/http_api.c::makapix_command_handler`)
   - `"swap_next"` command → `api_enqueue_swap_next()`
   - `"swap_back"` command → `api_enqueue_swap_back()`

### Auto-Swap (NOT user-initiated)

Auto-swap is triggered by the dwell timer in `play_scheduler_timer.c`:
- `dwell_timer_callback()` → `event_bus_emit_simple(P3A_EVENT_SWAP_NEXT)`
- This does **NOT** set processing-notification

### Key Distinction: User vs. Auto

| Source | Triggers Processing-Notification |
|--------|----------------------------------|
| Touch tap (left/right) | ✅ Yes |
| REST API swap_next/swap_back | ✅ Yes |
| MQTT swap_next/swap_back | ✅ Yes |
| Dwell timer expiry | ❌ No |
| Initial boot load | ❌ No |
| Channel refresh triggers | ❌ No |

### Render Pipeline Integration Point

The `display_render_task()` in `main/display_renderer.c` is the central rendering loop. After rendering each frame, it calls `fps_update_and_draw()` to overlay FPS. The processing-notification overlay would be drawn similarly, **after** the FPS overlay (so it's on top of everything).

```c
// Current flow in display_render_task():
// 1. Render animation/UI frame to back_buffer
// 2. fps_update_and_draw(back_buffer)          ← FPS overlay
// 3. esp_lcd_panel_draw_bitmap(...)            ← Send to LCD

// Proposed flow:
// 1. Render animation/UI frame to back_buffer
// 2. fps_update_and_draw(back_buffer)          ← FPS overlay
// 3. processing_notification_draw(back_buffer) ← NEW: Processing notification
// 4. esp_lcd_panel_draw_bitmap(...)            ← Send to LCD
```

## Implementation Plan

### Step 1: Add Config Store Functions for Notification Settings

**File**: `components/config_store/config_store.h` and `components/config_store/config_store.c`

Add getter/setter functions for notification settings following the existing pattern:

```c
// In config_store.h:
esp_err_t config_store_set_proc_notif_enabled(bool enable);
bool config_store_get_proc_notif_enabled(void);
esp_err_t config_store_set_proc_notif_size(uint8_t size);
uint8_t config_store_get_proc_notif_size(void);
```

Settings:
- `proc_notif_enabled`: boolean, default `true`
- `proc_notif_size`: uint8, default `32`, range `[8, 128]`

### Step 2: Add Processing-Notification State Variables

**File**: `main/display_renderer_priv.h` and `main/display_renderer.c`

Add state tracking for the notification:

```c
// Notification states
typedef enum {
    PROC_NOTIF_STATE_IDLE,       // Not showing
    PROC_NOTIF_STATE_PROCESSING, // Blue triangle - swap in progress
    PROC_NOTIF_STATE_FAILED      // Red triangle - swap failed, showing for 3 seconds
} proc_notif_state_t;

// In display_renderer_priv.h:
extern volatile proc_notif_state_t g_proc_notif_state;
extern volatile int64_t g_proc_notif_start_time_us;
extern volatile int64_t g_proc_notif_fail_time_us;

// In display_renderer.c:
volatile proc_notif_state_t g_proc_notif_state = PROC_NOTIF_STATE_IDLE;
volatile int64_t g_proc_notif_start_time_us = 0;
volatile int64_t g_proc_notif_fail_time_us = 0;
```

### Step 3: Create Notification Drawing Function

**File**: Create `main/display_processing_notification.c`

Implement the triangle drawing function:

```c
#define PROC_NOTIF_TIMEOUT_MS 5000      // Timeout for swap (triggers failure)
#define PROC_NOTIF_FAIL_DISPLAY_MS 3000 // How long to show red triangle

void processing_notification_update_and_draw(uint8_t *buffer);
```

Key implementation details:
- **Triangle shape**: For size N, draw pixel at `(lx, ly)` if `(N - 1 - lx) <= ly`
  - This creates a right triangle with the right angle at bottom-right
- **Checkerboard**: Draw if `(lx + ly) % 2 == 0`
- **Color**: Blue (0, 0, 255) for PROCESSING state, Red (255, 0, 0) for FAILED state
- **Size**: Read from config_store_get_proc_notif_size()
- **Enabled check**: Read from config_store_get_proc_notif_enabled()
- **State machine**:
  - IDLE: Don't draw
  - PROCESSING: Draw blue; if timeout (5s) exceeded, transition to FAILED
  - FAILED: Draw red; if 3 seconds elapsed since failure, transition to IDLE

### Step 4: Integrate into Render Loop

**File**: `main/display_renderer.c` in `display_render_task()`

Add the draw call after FPS overlay, but **only in animation mode** (not UI mode):

```c
// FPS overlay (from display_fps_overlay.c)
fps_update_and_draw(back_buffer);

// Processing notification overlay (only in animation mode)
if (!ui_mode) {
    processing_notification_update_and_draw(back_buffer);
}
```

This ensures the processing notification never appears during provisioning, OTA updates, or other UI modes.

### Step 5: Set State on User-Initiated Swaps

**Files**: Multiple entry points

Add explicit state setting at each user-initiated entry point. Each file must include the processing notification header.

1. **Touch** (`components/p3a_core/p3a_touch_router.c`):
   ```c
   #include "processing_notification.h"  // ADD at top of file
   
   // In handle_animation_playback():
   case P3A_TOUCH_EVENT_TAP_LEFT:
       proc_notif_start();  // ADD THIS
       if (app_lcd_cycle_animation_backward) {
           app_lcd_cycle_animation_backward();
       }
       return ESP_OK;
       
   case P3A_TOUCH_EVENT_TAP_RIGHT:
       proc_notif_start();  // ADD THIS
       if (app_lcd_cycle_animation) {
           app_lcd_cycle_animation();
       }
       return ESP_OK;
   ```

2. **HTTP API** (`components/http_api/http_api_rest.c`):
   ```c
   #include "processing_notification.h"  // ADD at top of file
   
   esp_err_t h_post_swap_next(httpd_req_t *req) {
       proc_notif_start();  // ADD THIS
       // ... existing implementation ...
   }
   
   esp_err_t h_post_swap_back(httpd_req_t *req) {
       proc_notif_start();  // ADD THIS
       // ... existing implementation ...
   }
   ```

3. **MQTT** (`components/http_api/http_api.c::makapix_command_handler`):
   ```c
   #include "processing_notification.h"  // ADD at top of file
   
   if (strcmp(command_type, "swap_next") == 0) {
       proc_notif_start();  // ADD THIS
       api_enqueue_swap_next();
   } else if (strcmp(command_type, "swap_back") == 0) {
       proc_notif_start();  // ADD THIS
       api_enqueue_swap_back();
   }
   ```

### Step 6: Clear State on Successful Buffer Swap

**File**: `main/animation_player_render.c` in `animation_player_render_frame_callback()`

Clear the state when the buffer swap completes successfully:

```c
#include "processing_notification.h"  // ADD at top of file

// Handle buffer swap
if (swap_requested && back_buffer_ready) {
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        // ... existing swap logic ...
        
        ESP_LOGI(TAG, "Buffers swapped: now playing %s", ...);
        
        // Clear processing notification - successful swap
        proc_notif_success();  // ADD THIS
        
        // ... rest of existing code ...
    }
}
```

### Step 7: Handle Failed Swaps (Red Indicator)

The state machine in `display_processing_notification.c` handles failures automatically:

1. **Timeout detection**: If swap takes longer than 5 seconds, transition to FAILED state
2. **Red indicator**: Draw red checkerboard triangle for 3 seconds
3. **Auto-clear**: After 3 seconds of showing red, return to IDLE state

The draw function handles all state transitions:

```c
void processing_notification_update_and_draw(uint8_t *buffer)
{
    // Check if feature is enabled
    if (!config_store_get_proc_notif_enabled()) {
        return;
    }
    
    int64_t now_us = esp_timer_get_time();
    
    // State machine
    switch (g_proc_notif_state) {
        case PROC_NOTIF_STATE_IDLE:
            return;  // Nothing to draw
            
        case PROC_NOTIF_STATE_PROCESSING:
            // Check for timeout (5 seconds)
            if ((now_us - g_proc_notif_start_time_us) > (PROC_NOTIF_TIMEOUT_MS * 1000LL)) {
                g_proc_notif_state = PROC_NOTIF_STATE_FAILED;
                g_proc_notif_fail_time_us = now_us;
                ESP_LOGW(TAG, "Processing notification timed out - swap failed");
            }
            // Draw blue triangle
            draw_checkerboard_triangle(buffer, 0, 0, 255);  // Blue
            break;
            
        case PROC_NOTIF_STATE_FAILED:
            // Check if 3 seconds have elapsed
            if ((now_us - g_proc_notif_fail_time_us) > (PROC_NOTIF_FAIL_DISPLAY_MS * 1000LL)) {
                g_proc_notif_state = PROC_NOTIF_STATE_IDLE;
                return;
            }
            // Draw red triangle
            draw_checkerboard_triangle(buffer, 255, 0, 0);  // Red
            break;
    }
}
```

### Step 8: Update CMakeLists.txt

**File**: `main/CMakeLists.txt`

Add the new source file:

```cmake
set(srcs
    "app_lcd_p4.c"
    ...
    "display_fps_overlay.c"
    "display_processing_notification.c"  # ADD THIS
    ...
)
```

## File Changes Summary

| File | Change Type | Description |
|------|-------------|-------------|
| `components/config_store/config_store.h` | Modify | Add proc_notif_enabled and proc_notif_size getter/setter declarations |
| `components/config_store/config_store.c` | Modify | Add proc_notif_enabled and proc_notif_size implementations |
| `main/display_renderer_priv.h` | Modify | Add state enum and extern declarations |
| `main/display_renderer.c` | Modify | Add state variables, integrate draw call in render loop |
| `main/display_processing_notification.c` | Create | New file with triangle drawing, state machine, NVS integration |
| `main/animation_player_render.c` | Modify | Call proc_notif_success() on buffer swap |
| `components/p3a_core/p3a_touch_router.c` | Modify | Call proc_notif_start() on touch tap events |
| `components/http_api/http_api_rest.c` | Modify | Call proc_notif_start() on swap_next/swap_back API calls |
| `components/http_api/http_api.c` | Modify | Call proc_notif_start() on MQTT swap commands |
| `main/CMakeLists.txt` | Modify | Add new source file |

## Testing Plan

### Manual Testing

1. **Touch tap test**: Tap right side of screen, verify blue checkerboard triangle appears immediately, disappears when new animation starts
2. **REST API test**: `curl -X POST http://p3a.local/action/swap_next`, verify same behavior
3. **MQTT test**: Send swap_next via Makapix, verify same behavior
4. **Auto-swap test**: Wait for dwell timer to expire, verify NO triangle appears
5. **Visibility test**: Test with various artwork backgrounds (light, dark, blue) to ensure checkerboard is visible
6. **Failed swap test**: Trigger a swap when no artwork is available, verify triangle turns red after 5s and disappears after 3 more seconds
7. **Size setting test**: Change proc_notif_size via config and verify triangle size changes
8. **Enable/disable test**: Toggle proc_notif_enabled and verify notification can be turned off

### Edge Cases

1. **Rapid taps**: Multiple taps in quick succession should not cause issues (state machine handles this)
2. **Failed swap**: Triangle turns red after 5 seconds, stays red for 3 seconds, then disappears
3. **UI mode**: Processing notification should not appear during UI mode (provisioning, OTA) — enforced by conditional in render loop
4. **Size bounds**: Verify size is clamped to [8, 128] range

## Open Questions for Discussion

1. ~~**Timeout**: Should there be a maximum display duration for the notification (e.g., 5 seconds) in case the swap fails silently?~~
   **RESOLVED**: Yes, implemented as a 5-second timeout that transitions to FAILED state.

2. ~~**Failed swap handling**: If the swap fails and we're showing the same animation, should the notification clear anyway?~~
   **RESOLVED**: Yes, failed swaps show red triangle for 3 seconds, then clear.

3. ~~**Color choice**: Blue was specified, but should it be configurable? Or should it use a contrasting color based on current frame?~~
   **RESOLVED**: Blue for processing, red for failure. Not configurable in initial implementation.

4. **Animation**: Should the checkerboard animate (e.g., invert pattern each frame) for more visibility?

5. **Position**: Bottom-right corner was specified. Should this respect screen rotation, or always be in the same physical corner?

## Estimated Implementation Time

- Core implementation: 2-3 hours
- Testing and refinement: 1 hour
- Total: 3-4 hours

---

*Document created: January 2026*
*Author: GitHub Copilot Coding Agent*
