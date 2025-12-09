# Artwork Comments Feature - Implementation Plan

## Document Status

**Last Updated**: 2025-12-09

**Changes from Original Plan**:
- Artwork comments is now a **sub-state of P3A_STATE_ANIMATION_PLAYBACK** (not a separate global state)
- Added support for three metadata scenarios: artworks with comments, artworks with zero comments, and artworks without metadata
- Integrated with current `p3a_state` system instead of creating separate state management
- Updated to reflect current codebase structure including `components/p3a_core/`, `components/channel_manager/`, etc.
- Uses existing `animation_metadata` from `playback_controller` to determine if sidecar JSON exists
- No modifications needed to `playback_controller` (no new playback source)

## Overview

This document provides a detailed implementation plan for adding the **Artwork Comments** feature to p3a. This feature allows users to view threaded comments on artworks by performing a 2-finger vertical swipe gesture.

## Background

### Current Global States
The p3a system currently has four main global states (defined in `components/p3a_core/include/p3a_state.h`):
1. **P3A_STATE_ANIMATION_PLAYBACK**: Normal animation playback from channels
   - Sub-states: `P3A_PLAYBACK_PLAYING`, `P3A_PLAYBACK_CHANNEL_MESSAGE`
2. **P3A_STATE_PROVISIONING**: Makapix Club registration
   - Sub-states: `P3A_PROV_STATUS`, `P3A_PROV_SHOW_CODE`, `P3A_PROV_CAPTIVE_AP_INFO`
3. **P3A_STATE_OTA**: Firmware updates
   - Sub-states: `P3A_OTA_CHECKING`, `P3A_OTA_DOWNLOADING`, `P3A_OTA_VERIFYING`, `P3A_OTA_FLASHING`, `P3A_OTA_PENDING_REBOOT`
4. **P3A_STATE_PICO8_STREAMING**: Real-time PICO-8 frame streaming

**Artwork-comments will be implemented as a sub-state of P3A_STATE_ANIMATION_PLAYBACK**, not as a separate global state. This is appropriate because:
- Comments are specific to an artwork being played
- Not all artworks have comment functionality (e.g., local SD card artworks without sidecar JSON files)
- It allows seamless transition back to playback when exiting comments mode

### Current Architecture

The p3a codebase has a well-structured graphics pipeline:

- **Display Renderer** (`display_renderer.c`): Core rendering loop that manages frame buffers and render modes
  - Currently supports two modes: `DISPLAY_RENDER_MODE_ANIMATION` and `DISPLAY_RENDER_MODE_UI`
  - Uses a callback system for frame rendering
  - Manages double-buffered frame output with vsync
  - Has upscale workers (2 tasks) that split screen rendering across two CPU cores

- **Animation Player** (`animation_player.c`): Handles animation playback
  - Uses the display renderer's animation mode
  - Implements parallel upscaling from native buffers to display buffers

- **UI System** (`ugfx_ui.c`): Renders UI overlays using µGFX library
  - Used for provisioning screens, OTA progress, and captive portal info
  - Renders directly to frame buffer at 720×720 resolution

- **Touch Input** (`app_touch.c`): Gesture recognition
  - Already handles tap, swipe (brightness), long-press (provisioning), and 2-finger rotation
  - Uses state machine for gesture detection

- **Playback Controller** (`playback_controller.c`): Manages playback sources
  - Tracks current animation metadata
  - Supports switching between animation and PICO-8 streaming modes

## Requirements Summary

### Artwork Metadata Status

The system must distinguish between three artwork metadata scenarios:

1. **Artwork with metadata and comments available**: Full comments UI with artwork metadata
   - Source: Makapix channels with server-provided sidecar JSON
   - Display: Full metadata header + comments section

2. **Artwork with metadata but zero comments**: Metadata present, server confirms no comments
   - Source: Makapix channels with sidecar JSON but empty comments array
   - Display: "No comments" message centered on screen

3. **Artwork without metadata**: No sidecar JSON available
   - Source: SD card animations without corresponding .json sidecar file
   - Display: "No artwork metadata" message centered on screen

### UI Layout

When user enters artwork-comments with 2-finger vertical swipe:

1. **Background**: Solid color (initially black, configurable at runtime)

2. **Top Section**: Artwork metadata (only if metadata available)
   - Artwork name
   - Artwork author
   - Artwork posted date
   - Count of each emoji reaction
   - Count of comments

3. **Comments Section**: Scrollable threaded comments (up to 2 levels deep)
   - Comment structure: `Comment A` → `Comment 1 to A` → `Comment X to Comment 1 to A`
   - Each comment displayed in a **comment-box** with rounded corners
   - Comment-box assembly uses **8 sprites**:
     - 4 corner sprites
     - 2 vertical border sprites (left, right)
     - 2 horizontal border sprites (top, bottom)
   - Comment-box styling:
     - Fill color: dark navy (configurable)
     - Author name: light pink text, top-left
     - Post date: below author name
     - Comment body: white text, below metadata

4. **Threaded Layout**:
   - Child comments go **inside** parent comment-box
   - Child comment-boxes are slightly smaller and offset to the right
   - Parent box size calculated from text body + space for all children

5. **Scrolling**: 1-finger vertical swipes scroll through comments

6. **Empty State Messages**:
   - "No comments" when metadata exists but comments array is empty
   - "No artwork metadata" when no sidecar JSON file exists for the animation

### Technical Requirements

1. **Performance**: Target 10 FPS (100ms frame duration) in comments mode
2. **Rendering Pipeline**: Use existing 2-worker architecture but repurpose for UI drawing instead of upscaling
3. **Viewport Clipping**: Workers must efficiently clip drawing to their assigned rectangle
4. **Incremental Updates**: Only redraw when scrolling or new comments arrive
5. **MQTT Integration**: Query comments from server when entering mode

## Implementation Plan

### Phase 1: Data Structures and State Management

#### 1.1 Define Comment Data Structures

Create `main/include/artwork_comments.h`:

```c
#ifndef ARTWORK_COMMENTS_H
#define ARTWORK_COMMENTS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Maximum nesting level for comments (0 = root, 1 = reply, 2 = reply to reply)
#define COMMENT_MAX_DEPTH 2

// Metadata availability status
typedef enum {
    METADATA_STATUS_UNKNOWN,          // Initial state, not yet checked
    METADATA_STATUS_NOT_AVAILABLE,    // No sidecar JSON file exists
    METADATA_STATUS_AVAILABLE_NO_COMMENTS,  // Metadata exists, zero comments
    METADATA_STATUS_AVAILABLE_WITH_COMMENTS // Metadata exists with comments
} metadata_status_t;

// Comment structure
typedef struct comment_s {
    uint32_t id;                      // Unique comment ID
    char *author_name;                // Author name (allocated)
    char *text;                       // Comment body text (allocated)
    time_t posted_time;               // Timestamp
    uint32_t parent_id;               // Parent comment ID (0 if root)
    uint8_t depth;                    // Nesting depth (0-2)
    
    // Child comments
    struct comment_s **children;      // Array of child comment pointers
    uint16_t child_count;             // Number of children
    
    // Layout info (computed during layout phase)
    int16_t box_x;                    // X position in page
    int16_t box_y;                    // Y position in page
    uint16_t box_width;               // Width of comment box
    uint16_t box_height;              // Height of comment box (includes children)
} comment_t;

// Artwork metadata for comments screen
// Note: This is separate from animation_metadata_t in channel_manager
// as it specifically stores server-provided social data
typedef struct {
    char *artwork_name;               // Artwork name (allocated)
    char *author_name;                // Author name (allocated)
    time_t posted_time;               // Posted timestamp
    uint16_t reaction_counts[5];     // Count for each emoji reaction
    uint16_t total_comments;          // Total comment count
} artwork_comments_metadata_t;

// Comments page state
typedef struct {
    metadata_status_t status;         // Current metadata status
    artwork_comments_metadata_t metadata; // Artwork metadata (only if available)
    comment_t **root_comments;        // Array of root-level comments
    uint16_t root_comment_count;      // Number of root comments
    
    // Layout state
    uint16_t page_height;             // Total height of comments page
    int16_t scroll_offset;            // Current scroll position
    
    // Loading state
    bool loading;                     // True if waiting for server data
    bool loaded;                      // True if server response received
} comments_page_t;

// Initialize comments system
esp_err_t artwork_comments_init(void);

// Deinitialize and free all resources
void artwork_comments_deinit(void);

// Enter comments mode for current artwork
// Checks animation_metadata from playback_controller to determine if sidecar exists
esp_err_t artwork_comments_enter(void);

// Exit comments mode (returns to playback)
void artwork_comments_exit(void);

// Check if currently in comments mode
bool artwork_comments_is_active(void);

// Scroll the comments page
void artwork_comments_scroll(int16_t delta_y);

// Get current comments page (for rendering)
const comments_page_t *artwork_comments_get_page(void);

#endif // ARTWORK_COMMENTS_H
```

#### 1.2 Add Comments Sub-state to P3A State Machine

**Instead of adding a new global state**, extend `components/p3a_core/include/p3a_state.h` to add a new sub-state to `P3A_STATE_ANIMATION_PLAYBACK`:

```c
/**
 * @brief Animation playback sub-states
 */
typedef enum {
    P3A_PLAYBACK_PLAYING,           ///< Normal animation display
    P3A_PLAYBACK_CHANNEL_MESSAGE,   ///< Displaying channel status message
    P3A_PLAYBACK_ARTWORK_COMMENTS,  ///< NEW: Viewing artwork comments
} p3a_playback_substate_t;
```

Add functions to enter/exit comments sub-state:
```c
/**
 * @brief Enter artwork comments sub-state
 * 
 * Transitions from PLAYING to ARTWORK_COMMENTS.
 * Requires that playback_controller has valid animation metadata.
 * 
 * @return ESP_OK if transition successful
 *         ESP_ERR_INVALID_STATE if not in ANIMATION_PLAYBACK state or no metadata
 */
esp_err_t p3a_state_enter_artwork_comments(void);

/**
 * @brief Exit artwork comments sub-state
 * 
 * Returns from ARTWORK_COMMENTS to PLAYING.
 */
void p3a_state_exit_artwork_comments(void);

/**
 * @brief Check if currently viewing artwork comments
 * 
 * @return true if in ARTWORK_COMMENTS sub-state
 */
bool p3a_state_is_artwork_comments(void);
```

This approach maintains consistency with the existing state machine architecture where:
- Provisioning and OTA are separate global states (they suspend playback entirely)
- PICO8_STREAMING is a separate global state (it replaces animation content)
- Comments viewing is contextual to the current artwork (stays within ANIMATION_PLAYBACK)

#### 1.3 Update Display Renderer Modes

Add new render mode to `main/include/display_renderer.h`:

```c
typedef enum {
    DISPLAY_RENDER_MODE_ANIMATION,  // Animation/streaming pipeline owns buffers
    DISPLAY_RENDER_MODE_UI,         // UI pipeline owns buffers (provisioning, OTA)
    DISPLAY_RENDER_MODE_COMMENTS    // NEW: Comments UI rendering (uses worker tasks)
} display_render_mode_t;
```

Add functions:
```c
/**
 * @brief Enter comments rendering mode
 * 
 * Blocks until render loop acknowledges mode switch.
 * 
 * @return ESP_OK on success
 */
esp_err_t display_renderer_enter_comments_mode(void);

/**
 * @brief Exit comments rendering mode
 * 
 * Returns to ANIMATION mode. Blocks until render loop acknowledges.
 */
void display_renderer_exit_comments_mode(void);

/**
 * @brief Check if currently in comments mode
 * 
 * @return true if comments mode active
 */
bool display_renderer_is_comments_mode(void);
```

The comments mode will reuse the existing upscale worker tasks (`g_upscale_worker_top` and `g_upscale_worker_bottom`) but repurpose them for UI rendering instead of upscaling operations.

### Phase 2: Touch Gesture Detection

#### 2.1 Add 2-Finger Vertical Swipe Detection

In `main/app_touch.c`, add new gesture state to the existing `gesture_state_t` enum:

```c
typedef enum {
    GESTURE_STATE_IDLE,
    GESTURE_STATE_TAP,
    GESTURE_STATE_BRIGHTNESS,
    GESTURE_STATE_LONG_PRESS_PENDING,
    GESTURE_STATE_ROTATION,
    GESTURE_STATE_TWO_FINGER_SWIPE    // NEW
} gesture_state_t;
```

Detection logic (integrate into existing touch handler):
- Detect when 2 fingers are touching (multi-touch support already exists)
- Track vertical movement of both fingers
- If both fingers move in same direction (up or down) by minimum threshold
- And horizontal movement is small
- Trigger comments sub-state entry/exit via `p3a_state_enter_artwork_comments()` / `p3a_state_exit_artwork_comments()`

**Swipe direction mapping**:
- **2-finger swipe UP**: Call `p3a_state_enter_artwork_comments()` to enter comments sub-state
- **2-finger swipe DOWN** (when in comments): Call `p3a_state_exit_artwork_comments()` to return to playback
- **1-finger swipe UP/DOWN** (when in comments): Call `artwork_comments_scroll()` instead of brightness adjustment

**Entry conditions check**:
Before entering comments mode, verify:
1. Current state is `P3A_STATE_ANIMATION_PLAYBACK` and sub-state is `P3A_PLAYBACK_PLAYING`
2. Use `playback_controller_has_animation_metadata()` to check if metadata might be available
3. If conditions not met, ignore the gesture or show brief error message

### Phase 3: Comment Box Rendering System

#### 3.1 Sprite System

Create `main/comments_sprites.c`:

Sprites needed:
- 4 corner sprites (top-left, top-right, bottom-left, bottom-right)
- 2 vertical border sprites (left edge, right edge)
- 2 horizontal border sprites (top edge, bottom edge)

Sprites stored as embedded binary data or generated procedurally.

Implementation approach:
```c
typedef struct {
    uint16_t width;
    uint16_t height;
    const uint8_t *rgba_data;  // RGBA pixel data
} sprite_t;

// Pre-defined sprites
extern const sprite_t comment_corner_tl;
extern const sprite_t comment_corner_tr;
extern const sprite_t comment_corner_bl;
extern const sprite_t comment_corner_br;
extern const sprite_t comment_border_h;  // 1-pixel tall, tiled horizontally
extern const sprite_t comment_border_v;  // 1-pixel wide, tiled vertically

// Draw a comment box at specified position with given dimensions
void draw_comment_box(uint8_t *fb, size_t stride,
                      int16_t x, int16_t y,
                      uint16_t width, uint16_t height,
                      uint32_t fill_color);
```

#### 3.2 Text Rendering

Leverage µGFX for text rendering:
- Use existing DejaVu Sans fonts
- Author name: light pink (`HTML2COLOR(0xFFB6C1)`)
- Comment body: white (`GFX_WHITE`)
- Post date: light gray

Create text wrapping function to calculate required box height:
```c
uint16_t calculate_text_height(const char *text, uint16_t box_width, gFont font);
```

### Phase 4: Layout Engine

#### 4.1 Comment Layout Algorithm

Create `main/comments_layout.c`:

```c
// Layout computation for a single comment and its children
void layout_comment(comment_t *comment, int16_t parent_x, int16_t parent_y,
                    uint16_t max_width, uint8_t depth);

// Layout all comments on the page
void layout_comments_page(comments_page_t *page);
```

**Layout rules**:
1. Root comments start at x=20, width=680 (with 20px margins)
2. Each depth level indents by 30px and reduces width by 60px
3. Comment box height = header (40px) + text height + children height + padding
4. Vertical spacing between sibling comments: 15px
5. Child comments start 10px below parent's text area
6. Maximum depth is 2 (enforced during data structure creation)

### Phase 5: Comments UI Drawing Workers

#### 5.1 Repurpose Worker Tasks

Currently: `g_upscale_worker_top` and `g_upscale_worker_bottom` handle upscaling.

New approach: When in comments mode, these workers become UI drawing workers.

Create `main/comments_render.c`:

```c
// Shared state for UI workers (similar to upscale shared state)
extern uint8_t *g_comments_dst_buffer;
extern const comments_page_t *g_comments_page;
extern int16_t g_comments_viewport_y;  // Scroll position
extern int g_comments_row_start_top;
extern int g_comments_row_end_top;
extern int g_comments_row_start_bottom;
extern int g_comments_row_end_bottom;

// Worker task entry points
void comments_ui_worker_top_task(void *arg);
void comments_ui_worker_bottom_task(void *arg);

// Render comments to specific row range
void render_comments_rows(uint8_t *fb, size_t stride,
                         const comments_page_t *page,
                         int16_t scroll_offset,
                         int row_start, int row_end);
```

#### 5.2 Viewport Clipping

Each worker draws only the comments visible in its assigned row range:

```c
// Check if comment box intersects with viewport row range
bool comment_intersects_rows(const comment_t *comment,
                            int16_t scroll_offset,
                            int row_start, int row_end);

// Clip drawing operations to assigned row range
void draw_comment_clipped(uint8_t *fb, size_t stride,
                         const comment_t *comment,
                         int16_t scroll_offset,
                         int row_start, int row_end);
```

**Clipping strategy**:
- Before drawing each comment, check if its Y range (adjusted for scroll) intersects worker's row range
- Only process comments that are at least partially visible
- Use scissor rectangle to clip individual drawing operations

### Phase 6: Integration with Display Renderer

#### 6.1 Modify `display_render_task`

In `main/display_renderer.c`, add new mode handling to the existing render loop:

```c
if (mode == DISPLAY_RENDER_MODE_COMMENTS) {
    // Clear buffer to background color
    uint32_t bg_color = comments_get_background_color();
    fill_buffer(back_buffer, bg_color, g_display_buffer_bytes);
    
    // Get comments page state
    const comments_page_t *page = artwork_comments_get_page();
    
    if (page->loading) {
        // Show loading indicator
        draw_loading_message(back_buffer, g_display_row_stride);
    } else if (page->loaded) {
        // Check metadata status and render accordingly
        switch (page->status) {
            case METADATA_STATUS_NOT_AVAILABLE:
                // Show "No artwork metadata" centered on screen
                draw_centered_message(back_buffer, g_display_row_stride, "No artwork metadata");
                break;
            
            case METADATA_STATUS_AVAILABLE_NO_COMMENTS:
                // Show metadata header + "No comments" message
                draw_metadata_header(back_buffer, g_display_row_stride, &page->metadata);
                draw_centered_message(back_buffer, g_display_row_stride, "No comments");
                break;
            
            case METADATA_STATUS_AVAILABLE_WITH_COMMENTS:
                // Set up shared state for workers
                g_comments_dst_buffer = back_buffer;
                g_comments_page = page;
                g_comments_viewport_y = page->scroll_offset;
                
                // Split screen between workers (same pattern as upscaling)
                const int mid_row = 720 / 2;  // Display height / 2
                g_comments_row_start_top = 0;
                g_comments_row_end_top = mid_row;
                g_comments_row_start_bottom = mid_row;
                g_comments_row_end_bottom = 720;  // Display height
                
                // Notify workers and wait for completion
                DISPLAY_MEMORY_BARRIER();
                xTaskNotify(g_upscale_worker_top, 1, eSetBits);
                xTaskNotify(g_upscale_worker_bottom, 1, eSetBits);
                
                // Wait for both workers (same pattern as existing upscale code)
                // ... (notification bits handling)
                break;
            
            default:
                // Unknown status - show error
                draw_centered_message(back_buffer, g_display_row_stride, "Error loading comments");
                break;
        }
    }
    
    frame_delay_ms = 100;  // 10 FPS
}
```

#### 6.2 Modify Worker Tasks

Extend worker tasks to handle both upscaling and UI drawing:

```c
void display_upscale_worker_top_task(void *arg) {
    while (true) {
        xTaskNotifyWait(0, UINT32_MAX, &notification_value, portMAX_DELAY);
        DISPLAY_MEMORY_BARRIER();
        
        display_render_mode_t mode = g_display_mode_active;
        
        if (mode == DISPLAY_RENDER_MODE_ANIMATION) {
            // Existing upscaling code
            blit_upscaled_rows(...);
        } else if (mode == DISPLAY_RENDER_MODE_COMMENTS) {
            // NEW: Render comments UI
            render_comments_rows(g_comments_dst_buffer, g_display_row_stride,
                               g_comments_page, g_comments_viewport_y,
                               g_comments_row_start_top, g_comments_row_end_top);
        }
        
        DISPLAY_MEMORY_BARRIER();
        g_upscale_worker_top_done = true;
        xTaskNotify(g_upscale_main_task, (1UL << 0), eSetBits);
    }
}
```

### Phase 7: MQTT Protocol for Comments

#### 7.1 MQTT Topic Structure

Add to `components/makapix/include/makapix_mqtt.h` or create new header:

```c
// Request comments for current artwork
// Publish to: makapix/device/{device_id}/artwork/comments/request
// Payload: {"artwork_id": "<id>", "filepath": "/path/to/file.webp"}

// Receive comments response
// Subscribe to: makapix/device/{device_id}/artwork/comments/response
// Payload: {
//   "status": "success" | "no_metadata" | "no_comments",
//   "artwork": {
//     "id": "<id>",
//     "name": "Artwork Name",
//     "author": "Author Name",
//     "posted": "2024-01-15T10:30:00Z",
//     "reactions": {"like": 42, "love": 15, "wow": 3, "sad": 0, "angry": 1},
//     "comment_count": 7
//   },
//   "comments": [
//     {
//       "id": 101,
//       "author": "User1",
//       "text": "Great artwork!",
//       "posted": "2024-01-16T14:20:00Z",
//       "parent_id": 0
//     },
//     {
//       "id": 102,
//       "author": "User2",
//       "text": "Thanks!",
//       "posted": "2024-01-16T15:00:00Z",
//       "parent_id": 101
//     }
//   ]
// }
// 
// Response status values:
// - "success": Full metadata and comments provided
// - "no_metadata": Artwork not found in server database (use METADATA_STATUS_NOT_AVAILABLE)
// - "no_comments": Artwork exists but has zero comments (use METADATA_STATUS_AVAILABLE_NO_COMMENTS)
```

#### 7.2 MQTT Handler Implementation

Create `components/makapix/makapix_comments.c`:

```c
/**
 * @brief Request comments for the current artwork
 * 
 * Uses animation_metadata from playback_controller to get filepath.
 * Publishes MQTT request to server.
 * 
 * @return ESP_OK on success
 *         ESP_ERR_INVALID_STATE if no artwork is playing or no metadata
 */
esp_err_t makapix_request_comments(void);

/**
 * @brief Parse comments response from server
 * 
 * Updates artwork_comments state based on response.
 * 
 * @param json_payload JSON response string
 * @return ESP_OK on success
 */
esp_err_t makapix_parse_comments_response(const char *json_payload);
```

Parser must:
1. Parse JSON response and check "status" field
2. Set appropriate metadata_status_t based on status
3. If status is "success", build tree structure of comments
4. Enforce max depth of 2
5. Allocate memory for all strings
6. Call `layout_comments_page()` after building tree
7. If status is "no_metadata" or "no_comments", set status accordingly

### Phase 8: State Machine Integration

#### 8.1 State Transition Logic

In `components/p3a_core/p3a_state.c`, implement the new sub-state functions:

```c
esp_err_t p3a_state_enter_artwork_comments(void)
{
    // Take mutex
    if (xSemaphoreTake(s_state.mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    // Verify we're in ANIMATION_PLAYBACK state
    if (s_state.current_state != P3A_STATE_ANIMATION_PLAYBACK) {
        xSemaphoreGive(s_state.mutex);
        ESP_LOGW(TAG, "Cannot enter comments - not in ANIMATION_PLAYBACK");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Verify we're in PLAYING sub-state
    if (s_state.playback_substate != P3A_PLAYBACK_PLAYING) {
        xSemaphoreGive(s_state.mutex);
        ESP_LOGW(TAG, "Cannot enter comments - not in PLAYING sub-state");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Transition to ARTWORK_COMMENTS sub-state
    s_state.playback_substate = P3A_PLAYBACK_ARTWORK_COMMENTS;
    xSemaphoreGive(s_state.mutex);
    
    // Call artwork_comments module to initialize and load data
    esp_err_t ret = artwork_comments_enter();
    if (ret != ESP_OK) {
        // Revert sub-state on failure
        xSemaphoreTake(s_state.mutex, portMAX_DELAY);
        s_state.playback_substate = P3A_PLAYBACK_PLAYING;
        xSemaphoreGive(s_state.mutex);
        return ret;
    }
    
    // Switch display renderer to comments mode
    display_renderer_enter_comments_mode();
    
    ESP_LOGI(TAG, "Entered ARTWORK_COMMENTS sub-state");
    return ESP_OK;
}

void p3a_state_exit_artwork_comments(void)
{
    // Take mutex
    xSemaphoreTake(s_state.mutex, portMAX_DELAY);
    
    if (s_state.current_state != P3A_STATE_ANIMATION_PLAYBACK ||
        s_state.playback_substate != P3A_PLAYBACK_ARTWORK_COMMENTS) {
        xSemaphoreGive(s_state.mutex);
        ESP_LOGW(TAG, "Not in ARTWORK_COMMENTS sub-state");
        return;
    }
    
    // Transition back to PLAYING sub-state
    s_state.playback_substate = P3A_PLAYBACK_PLAYING;
    xSemaphoreGive(s_state.mutex);
    
    // Clean up comments module
    artwork_comments_exit();
    
    // Switch display renderer back to animation mode
    display_renderer_exit_comments_mode();
    
    ESP_LOGI(TAG, "Exited ARTWORK_COMMENTS sub-state");
}

bool p3a_state_is_artwork_comments(void)
{
    bool result = false;
    if (xSemaphoreTake(s_state.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        result = (s_state.current_state == P3A_STATE_ANIMATION_PLAYBACK &&
                  s_state.playback_substate == P3A_PLAYBACK_ARTWORK_COMMENTS);
        xSemaphoreGive(s_state.mutex);
    }
    return result;
}
```

Note: This replaces the need for a separate comments monitor task. The state is managed through the p3a_state system.

#### 8.2 Touch Handler Integration

In `main/app_touch.c`, integrate with existing touch gesture handler:

```c
// In the touch event processing function:
if (/* 2-finger vertical swipe detected */) {
    // Check if we're in a state that allows comments
    p3a_state_t current_state = p3a_state_get();
    
    if (current_state == P3A_STATE_ANIMATION_PLAYBACK) {
        if (p3a_state_is_artwork_comments()) {
            // Exit comments mode (2-finger swipe down)
            ESP_LOGI(TAG, "Exiting artwork comments mode");
            p3a_state_exit_artwork_comments();
        } else {
            // Try to enter comments mode (2-finger swipe up)
            // First check if metadata is available
            if (playback_controller_has_animation_metadata()) {
                ESP_LOGI(TAG, "Entering artwork comments mode");
                esp_err_t ret = p3a_state_enter_artwork_comments();
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to enter comments mode: %s", esp_err_to_name(ret));
                    // Optionally show brief error message via UI
                }
            } else {
                ESP_LOGI(TAG, "Cannot enter comments - no animation metadata");
                // Optionally show brief "No metadata" message
            }
        }
    }
}
```

When in comments mode, remap 1-finger vertical swipes to scrolling:

```c
// In existing brightness/swipe gesture handler:
if (gesture_state == GESTURE_STATE_BRIGHTNESS) {
    if (p3a_state_is_artwork_comments()) {
        // Repurpose brightness gesture as scrolling when in comments
        int16_t scroll_delta = -delta_y;  // Invert Y for natural scrolling
        artwork_comments_scroll(scroll_delta);
        // Don't update brightness
    } else {
        // Normal brightness control
        // ... existing brightness code ...
    }
}
```

Touch routing strategy:
- Check `p3a_state_is_artwork_comments()` to determine if gestures should be routed to comments
- 2-finger vertical swipe: Toggle comments mode (enter/exit)
- 1-finger vertical swipe: Scroll comments (when in comments mode) or brightness (when in playback)
- Other gestures (tap, long-press, rotation) remain unchanged or disabled in comments mode

### Phase 9: Configuration System

#### 9.1 Runtime Configuration

Add to `components/config_store/include/config_store.h` or `components/config_store/config_store.h`:

```c
// Background color for comments screen
uint32_t config_store_get_comments_bg_color(void);
void config_store_set_comments_bg_color(uint32_t color);

// Comment box fill color
uint32_t config_store_get_comment_box_color(void);
void config_store_set_comment_box_color(uint32_t color);
```

Default values:
- Background: Black (`0x000000`)
- Comment box fill: Dark navy (`0x001F3F`)

## Implementation Sequence

### Step 1: Foundation (Days 1-2)
- [ ] Create data structures in `main/include/artwork_comments.h` with metadata_status_t enum
- [ ] Implement basic state management in `main/artwork_comments.c`
- [ ] Add `P3A_PLAYBACK_ARTWORK_COMMENTS` sub-state to `components/p3a_core/include/p3a_state.h`
- [ ] Implement state transition functions in `components/p3a_core/p3a_state.c`
- [ ] Add `DISPLAY_RENDER_MODE_COMMENTS` to `main/include/display_renderer.h`
- [ ] Implement display mode functions in `main/display_renderer.c`

### Step 2: Touch Input (Day 3)
- [ ] Add `GESTURE_STATE_TWO_FINGER_SWIPE` to `main/app_touch.c`
- [ ] Implement 2-finger vertical swipe detection
- [ ] Integrate with `p3a_state_enter_artwork_comments()` / `p3a_state_exit_artwork_comments()`
- [ ] Add metadata availability check before entering comments
- [ ] Add gesture routing logic for comments mode (scroll vs brightness)
- [ ] Test gesture recognition

### Step 3: Rendering Core (Days 4-5)
- [ ] Create sprite system in `main/comments_sprites.c`
- [ ] Implement comment box drawing
- [ ] Add text rendering and wrapping using µGFX
- [ ] Implement empty state messages ("No comments", "No artwork metadata")
- [ ] Test individual component rendering

### Step 4: Layout Engine (Days 6-7)
- [ ] Create `main/comments_layout.c`
- [ ] Implement layout algorithm for threaded comments
- [ ] Handle nested comments correctly (max depth 2)
- [ ] Calculate page height
- [ ] Test layout with mock data

### Step 5: Worker Integration (Days 8-9)
- [ ] Extend worker tasks in `main/display_renderer.c` for comments UI rendering
- [ ] Add shared state variables for comments mode
- [ ] Implement viewport clipping for parallel rendering
- [ ] Implement metadata status-based rendering paths
- [ ] Optimize drawing performance
- [ ] Test split-screen rendering

### Step 6: MQTT Integration (Days 10-11)
- [ ] Define MQTT protocol in `components/makapix/include/makapix_mqtt.h` with status field
- [ ] Create `components/makapix/makapix_comments.c`
- [ ] Implement `makapix_request_comments()` using playback_controller metadata
- [ ] Implement JSON parsing with status handling ("success", "no_metadata", "no_comments")
- [ ] Build comment tree structure with max depth enforcement
- [ ] Test with mock server responses

### Step 7: State Machine Integration (Days 12-13)
- [ ] Wire up all state transition functions
- [ ] Connect touch gestures to state transitions
- [ ] Implement scrolling in comments mode
- [ ] Test transitions between PLAYING and ARTWORK_COMMENTS sub-states
- [ ] Test with SD card artworks (no metadata scenario)
- [ ] Test with Makapix artworks (metadata + comments scenarios)
- [ ] Test end-to-end flow

### Step 8: Polish (Days 14-15)
- [ ] Add loading indicators
- [ ] Handle error cases (network failure, malformed JSON)
- [ ] Test all three metadata scenarios thoroughly
- [ ] Performance optimization
- [ ] Memory leak testing
- [ ] Documentation updates

## Testing Strategy

### Unit Tests
- Layout algorithm with various comment depths
- Comment box rendering at different sizes
- Viewport clipping edge cases
- JSON parsing for malformed data

### Integration Tests
- Enter/exit comments sub-state from PLAYING sub-state
- Verify state transitions are blocked when not in ANIMATION_PLAYBACK
- Scrolling behavior in comments mode
- MQTT request/response cycle with different status values
- Test all three metadata scenarios:
  1. Artwork with comments (METADATA_STATUS_AVAILABLE_WITH_COMMENTS)
  2. Artwork with zero comments (METADATA_STATUS_AVAILABLE_NO_COMMENTS)
  3. SD card artwork without sidecar (METADATA_STATUS_NOT_AVAILABLE)
- State persistence when entering/exiting comments
- Gesture routing in comments mode vs. playback mode

### Performance Tests
- Frame time measurement (should be ≤100ms)
- Memory usage in comments mode
- Worker task utilization
- Cache coherency (no visual glitches)

### User Experience Tests
- Gesture recognition reliability
- Smooth scrolling
- Readable text at all sizes
- Proper nested comment visualization

## Memory Considerations

**Estimated Memory Usage:**
- Comment structure: ~50 bytes per comment
- Text data: Variable (average ~200 bytes per comment)
- Tree structure: Pointers (8 bytes × child_count)
- For 100 comments: ~25 KB

**Optimization strategies:**
1. Use arena allocator for comment tree
2. Limit maximum comments per page (e.g., 100)
3. Free old comments when entering new artwork
4. Consider pagination for very long comment threads

## Open Questions

1. **Sprite format**: Should we use embedded PNGs or procedurally generate rounded corners?
   - **Recommendation**: Procedural generation is more flexible and uses less flash

2. **Font sizes**: What font sizes for author name vs. comment body?
   - **Recommendation**: Author name 16pt, comment body 14pt, date 12pt

3. **Color scheme**: Should colors be fully configurable or just background?
   - **Recommendation**: Start with background + box color configurable, expand later

4. **Error handling**: What to show if MQTT request fails?
   - **Recommendation**: Show "Comments unavailable" message, allow retry

5. **Empty state**: What to show if artwork has no comments?
   - **Answer**: Implemented via metadata_status_t enum - "No comments" for zero comments, "No artwork metadata" for missing sidecar

6. **Metadata handling**: How to distinguish between no sidecar file vs. zero comments?
   - **Answer**: Use animation_metadata.has_metadata from playback_controller to check for sidecar, then MQTT response status field determines if comments exist

## Future Enhancements

- **Comment posting**: Allow users to post comments from device (requires keyboard UI)
- **Reactions**: Allow users to add emoji reactions
- **Profile pictures**: Show author avatars if available
- **Rich text**: Support for bold, italic, links in comment text
- **Image attachments**: Display inline images in comments
- **Infinite scroll**: Load more comments as user scrolls
- **Search/filter**: Find specific comments or filter by author
- **Notification badges**: Show new comment indicators

## References

- Current state machine: `components/p3a_core/include/p3a_state.h` and `components/p3a_core/p3a_state.c`
- Animation metadata system: `components/channel_manager/include/animation_metadata.h`
- Playback controller: `main/playback_controller.c` and `main/include/playback_controller.h`
- Touch gesture system: `main/app_touch.c` and `main/include/app_touch.h`
- Display renderer architecture: `main/display_renderer.c` and `main/include/display_renderer.h`
- Display renderer private API: `main/display_renderer_priv.h`
- UI rendering: `main/ugfx_ui.c` and `main/include/ugfx_ui.h`
- MQTT implementation: `components/makapix/` (makapix_mqtt.c and related files)
- Worker task pattern: `display_upscale_worker_*_task` functions in display_renderer.c

## Appendix A: File Structure

New files to create:
```
main/
  include/
    artwork_comments.h           - Public API with metadata_status_t
    comments_render.h            - Rendering API
    comments_layout.h            - Layout API
  artwork_comments.c             - Core state management and entry point
  comments_render.c              - UI rendering implementation
  comments_layout.c              - Layout algorithm
  comments_sprites.c             - Sprite data and drawing

components/makapix/
  include/
    makapix_comments.h           - MQTT comment API
  makapix_comments.c             - MQTT comment fetching and parsing

components/config_store/
  - Add color configuration APIs to existing files
```

Modified files:
```
components/p3a_core/
  include/
    p3a_state.h                  - Add P3A_PLAYBACK_ARTWORK_COMMENTS sub-state
  p3a_state.c                    - Implement state transition functions

main/
  app_touch.c                    - Add 2-finger swipe detection and routing
  display_renderer.c             - Add DISPLAY_RENDER_MODE_COMMENTS handling
  display_renderer_priv.h        - Add comments worker shared state variables

main/include/
  display_renderer.h             - Add comments mode APIs
```

**Note**: Unlike the original plan, we do NOT modify:
- `playback_controller.h` or `playback_controller.c` (no new playback source needed)
- `p3a_main.c` (no separate monitor task needed, uses p3a_state system)

## Appendix B: Color Definitions

Suggested color palette (RGB888 → RGB565):

```c
#define COMMENT_BG_DEFAULT      0x000000  // Black
#define COMMENT_BOX_FILL       0x001F3F  // Dark navy
#define COMMENT_AUTHOR_COLOR   0xFFB6C1  // Light pink
#define COMMENT_TEXT_COLOR     0xFFFFFF  // White
#define COMMENT_DATE_COLOR     0xCCCCCC  // Light gray
#define COMMENT_BORDER_COLOR   0x3A5F7D  // Muted blue-gray
```

RGB565 conversion:
```c
static inline uint16_t rgb888_to_rgb565(uint32_t rgb888) {
    uint8_t r = (rgb888 >> 16) & 0xFF;
    uint8_t g = (rgb888 >> 8) & 0xFF;
    uint8_t b = rgb888 & 0xFF;
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
```

## Document History

- **2024-12-08**: Initial plan created based on codebase analysis
- **2025-12-09**: Major revision to align with current codebase state:
  - Changed from global state to sub-state of `P3A_STATE_ANIMATION_PLAYBACK`
  - Added `metadata_status_t` enum to handle three artwork scenarios
  - Updated all file paths to reflect current component structure
  - Integrated with `p3a_state` system instead of separate state management
  - Removed `playback_controller` modifications (not needed)
  - Updated touch handling to use `p3a_state` functions
  - Added clarification on sidecar JSON vs. server metadata
  - Updated MQTT protocol with status field for better error handling
  - Revised implementation sequence and testing strategy
