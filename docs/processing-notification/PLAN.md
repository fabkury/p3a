# Processing Notification Implementation Plan

## Overview

This document outlines the implementation plan for **processing-notification**, a visual feedback mechanism that provides immediate acknowledgment to users when p3a receives a user-initiated animation swap command.

## Feature Definition

### What is Processing-Notification?

A **32×32 pixel blue square** drawn in the **bottom-right corner** of the display using a **checkerboard pattern** (every other pixel), providing immediate visual feedback that a user-initiated swap command has been received and is being processed.

### Visual Specification

- **Size**: 32×32 pixels
- **Position**: Bottom-right corner, perfectly aligned (x: 688–719, y: 688–719 on the 720×720 display)
- **Color**: Blue (`0x0000FF` in RGB888, or appropriate RGB565 equivalent)
- **Pattern**: Checkerboard pattern using local coordinates — for each pixel at local position `(lx, ly)` where `lx, ly ∈ [0, 31]`, the pixel is drawn if `(lx + ly) % 2 == 0`, otherwise skipped. This ensures the pattern is consistent regardless of screen position.
- **Purpose**: The checkerboard pattern ensures the notification is visible regardless of background content

### Lifecycle

```
┌─────────────────────────────────────────────────────────────────────────┐
│ User issues swap command (touchscreen tap, MQTT command, REST API)      │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ processing-notification flag is SET                                      │
│ (immediately, in the same context as receiving the command)              │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ Next render frame draws the blue checkerboard square                     │
│ (display_render_task checks flag and overlays if set)                    │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ Animation loader fetches/decodes new artwork                             │
│ (processing-notification continues to display on each frame)             │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ Buffer swap occurs (s_front_buffer ↔ s_back_buffer)                      │
│ processing-notification flag is CLEARED                                  │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ First frame of new animation renders WITHOUT the notification            │
└─────────────────────────────────────────────────────────────────────────┘
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

### Step 1: Add Processing-Notification State Flag

**File**: `main/display_renderer_priv.h` and `main/display_renderer.c`

Add a volatile flag that tracks whether processing-notification should be displayed:

```c
// In display_renderer_priv.h (add after the g_rotation_in_progress declaration, around line 110):
extern volatile bool g_processing_notification_active;

// In display_renderer.c (add after g_rotation_in_progress definition, around line 75):
volatile bool g_processing_notification_active = false;
```

**Thread Safety Note**: The `volatile` keyword is sufficient here because:
1. The flag is a simple boolean with atomic read/write operations on ESP32
2. There's no critical section that requires read-modify-write consistency
3. The worst case of a race condition is the notification appearing one frame early/late, which is acceptable
4. The flag is only ever set to `true` (never toggled), and cleared in a single location

### Step 2: Create Notification Drawing Function

**File**: Create `main/display_processing_notification.c`

Implement the checkerboard drawing function (similar pattern to `display_fps_overlay.c`):

```c
// Draw processing notification overlay (32x32 blue checkerboard, bottom-right)
void processing_notification_update_and_draw(uint8_t *buffer);
```

Key implementation details:
- Use same pixel drawing approach as `fps_draw_pixel()` in `display_fps_overlay.c`
- Position: `x ∈ [EXAMPLE_LCD_H_RES - 32, EXAMPLE_LCD_H_RES - 1]`, `y ∈ [EXAMPLE_LCD_V_RES - 32, EXAMPLE_LCD_V_RES - 1]`
- Checkerboard: Use local coordinates `(lx, ly)` where `lx = x - (EXAMPLE_LCD_H_RES - 32)`, draw if `(lx + ly) % 2 == 0`
- Color: Blue (0, 0, 255)
- Check `g_processing_notification_active` before drawing
- **UI Mode Check**: Also check that we're not in UI mode (`g_display_mode_active != DISPLAY_RENDER_MODE_UI`)

### Step 3: Integrate into Render Loop

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

### Step 4: Set Flag on User-Initiated Swaps

**Files**: Multiple entry points

#### Option A: Centralized in `animation_player_cycle_animation()`

Since both touch and API commands eventually call `animation_player_cycle_animation()`, this is a good centralization point:

**File**: `main/animation_player.c`

```c
void animation_player_cycle_animation(bool forward)
{
    // Set processing notification flag IMMEDIATELY
    g_processing_notification_active = true;
    
    // ... existing implementation ...
}
```

However, API commands may also go through `play_scheduler_next/prev` directly, so we need additional hooks.

#### Option B: Mark at API Entry Points

Add explicit flag setting at each user-initiated entry point. Note: Each file must include `"display_renderer_priv.h"` to access `g_processing_notification_active`.

1. **Touch** (`components/p3a_core/p3a_touch_router.c`):
   ```c
   #include "display_renderer_priv.h"  // ADD at top of file
   
   // In handle_animation_playback():
   case P3A_TOUCH_EVENT_TAP_LEFT:
       g_processing_notification_active = true;  // ADD THIS
       if (app_lcd_cycle_animation_backward) {
           app_lcd_cycle_animation_backward();
       }
       return ESP_OK;
       
   case P3A_TOUCH_EVENT_TAP_RIGHT:
       g_processing_notification_active = true;  // ADD THIS
       if (app_lcd_cycle_animation) {
           app_lcd_cycle_animation();
       }
       return ESP_OK;
   ```

2. **HTTP API** (`components/http_api/http_api_rest.c`):
   ```c
   #include "display_renderer_priv.h"  // ADD at top of file
   
   esp_err_t h_post_swap_next(httpd_req_t *req) {
       g_processing_notification_active = true;  // ADD THIS
       // ... existing implementation ...
   }
   
   esp_err_t h_post_swap_back(httpd_req_t *req) {
       g_processing_notification_active = true;  // ADD THIS
       // ... existing implementation ...
   }
   ```

3. **MQTT** (`components/http_api/http_api.c::makapix_command_handler`):
   ```c
   #include "display_renderer_priv.h"  // ADD at top of file (if not already present)
   
   if (strcmp(command_type, "swap_next") == 0) {
       g_processing_notification_active = true;  // ADD THIS
       api_enqueue_swap_next();
   } else if (strcmp(command_type, "swap_back") == 0) {
       g_processing_notification_active = true;  // ADD THIS
       api_enqueue_swap_back();
   }
   ```

#### Recommendation: Use Option B

Option B is more explicit and ensures the flag is set as early as possible in the call chain, guaranteeing the notification appears on the very next rendered frame.

### Step 5: Clear Flag on Buffer Swap

**File**: `main/animation_player_render.c` in `animation_player_render_frame_callback()`

Clear the flag when the buffer swap completes. Note: The flag is cleared at the moment of swap, meaning the notification will NOT appear in the same frame as the first frame of the new animation. This is correct because the buffer swap prepares the new content, and on the very next render cycle, the new animation's frame (without notification) is sent to the LCD.

```c
// Handle buffer swap
if (swap_requested && back_buffer_ready) {
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        // ... existing swap logic ...
        
        ESP_LOGI(TAG, "Buffers swapped: now playing %s", ...);
        
        // Clear processing notification - buffer swap complete, next render will show new animation
        g_processing_notification_active = false;  // ADD THIS
        
        // ... rest of existing code ...
    }
}
```

### Step 5b: Add Timeout for Failed Swaps

**File**: `main/display_processing_notification.c`

To handle edge cases where a swap fails silently, add a timeout mechanism:

```c
static int64_t s_notification_start_time_us = 0;
#define PROCESSING_NOTIFICATION_TIMEOUT_MS 5000

void processing_notification_update_and_draw(uint8_t *buffer)
{
    if (!g_processing_notification_active) {
        s_notification_start_time_us = 0;  // Reset timer when inactive
        return;
    }
    
    // Start timeout tracking
    int64_t now_us = esp_timer_get_time();
    if (s_notification_start_time_us == 0) {
        s_notification_start_time_us = now_us;
    }
    
    // Auto-clear after timeout (failed swap protection)
    if ((now_us - s_notification_start_time_us) > (PROCESSING_NOTIFICATION_TIMEOUT_MS * 1000)) {
        g_processing_notification_active = false;
        s_notification_start_time_us = 0;
        ESP_LOGW("proc_notif", "Processing notification timed out (possible failed swap)");
        return;
    }
    
    // Draw the notification...
}
```

### Step 6: Update CMakeLists.txt

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

### Step 7: Add Header Declaration

**File**: `main/display_renderer_priv.h`

```c
// Processing notification overlay (display_processing_notification.c)
void processing_notification_update_and_draw(uint8_t *buffer);
```

## File Changes Summary

| File | Change Type | Description |
|------|-------------|-------------|
| `main/display_renderer_priv.h` | Modify | Add `g_processing_notification_active` extern and function declaration |
| `main/display_renderer.c` | Modify | Add flag variable, integrate draw call in render loop |
| `main/display_processing_notification.c` | Create | New file with checkerboard drawing and timeout implementation |
| `main/animation_player_render.c` | Modify | Clear flag on buffer swap |
| `components/p3a_core/p3a_touch_router.c` | Modify | Set flag on touch tap events, add header include |
| `components/http_api/http_api_rest.c` | Modify | Set flag on swap_next/swap_back API calls, add header include |
| `components/http_api/http_api.c` | Modify | Set flag on MQTT swap commands, add header include |
| `main/CMakeLists.txt` | Modify | Add new source file |

## Testing Plan

### Manual Testing

1. **Touch tap test**: Tap right side of screen, verify blue checkerboard appears immediately, disappears when new animation starts
2. **REST API test**: `curl -X POST http://p3a.local/action/swap_next`, verify same behavior
3. **MQTT test**: Send swap_next via Makapix, verify same behavior
4. **Auto-swap test**: Wait for dwell timer to expire, verify NO blue checkerboard appears
5. **Visibility test**: Test with various artwork backgrounds (light, dark, blue) to ensure checkerboard is visible
6. **Challenging background test**: Test with artworks that have blue elements or checkerboard-like patterns in the corner area

### Edge Cases

1. **Rapid taps**: Multiple taps in quick succession should not cause issues (flag is idempotent)
2. **Failed swap**: If swap fails (e.g., no next animation), notification should clear after timeout (5 seconds)
3. **UI mode**: Processing notification should not appear during UI mode (provisioning, OTA) — enforced by conditional in render loop
4. **Timeout verification**: Verify the 5-second timeout clears the notification if swap fails

## Open Questions for Discussion

1. ~~**Timeout**: Should there be a maximum display duration for the notification (e.g., 5 seconds) in case the swap fails silently?~~
   **RESOLVED**: Yes, implemented as a 5-second timeout in Step 5b.

2. ~~**Failed swap handling**: If the swap fails and we're showing the same animation, should the notification clear anyway?~~
   **RESOLVED**: Yes, the timeout mechanism ensures the notification clears even on failed swaps.

3. **Color choice**: Blue was specified, but should it be configurable? Or should it use a contrasting color based on current frame?

4. **Animation**: Should the checkerboard animate (e.g., invert pattern each frame) for more visibility?

5. **Position**: Bottom-right corner was specified. Should this respect screen rotation, or always be in the same physical corner?

## Estimated Implementation Time

- Core implementation: 1-2 hours
- Testing and refinement: 1 hour
- Total: 2-3 hours

---

*Document created: January 2026*
*Author: GitHub Copilot Coding Agent*
