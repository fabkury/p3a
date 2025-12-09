# Artwork Comments Feature - Implementation Plan

## Overview

This document provides a detailed implementation plan for adding the **Artwork Comments** feature to p3a. This feature allows users to view threaded comments on artworks by performing a 2-finger vertical swipe gesture.

## Background

### Current Global States
The p3a system currently has three main global states:
1. **Animation-playback** (paused or running)
2. **Provisioning** (Makapix Club registration)
3. **OTA-update** (firmware updates)

We need to add a fourth state: **Artwork-comments**

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

### UI Layout

When user enters artwork-comments with 2-finger vertical swipe:

1. **Background**: Solid color (initially black, configurable at runtime)

2. **Top Section**: Artwork metadata
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
typedef struct {
    char *artwork_name;               // Artwork name (allocated)
    char *author_name;                // Author name (allocated)
    time_t posted_time;               // Posted timestamp
    uint16_t reaction_counts[5];     // Count for each emoji reaction
    uint16_t total_comments;          // Total comment count
} artwork_metadata_t;

// Comments page state
typedef struct {
    artwork_metadata_t metadata;      // Artwork metadata
    comment_t **root_comments;        // Array of root-level comments
    uint16_t root_comment_count;      // Number of root comments
    
    // Layout state
    uint16_t page_height;             // Total height of comments page
    int16_t scroll_offset;            // Current scroll position
    
    // Loading state
    bool loading;                     // True if waiting for server data
    bool loaded;                      // True if comments have arrived
} comments_page_t;

// Initialize comments system
esp_err_t artwork_comments_init(void);

// Deinitialize and free all resources
void artwork_comments_deinit(void);

// Enter comments mode for current artwork
esp_err_t artwork_comments_enter(void);

// Exit comments mode
void artwork_comments_exit(void);

// Check if currently in comments mode
bool artwork_comments_is_active(void);

// Scroll the comments page
void artwork_comments_scroll(int16_t delta_y);

// Get current comments page (for rendering)
const comments_page_t *artwork_comments_get_page(void);

#endif // ARTWORK_COMMENTS_H
```

#### 1.2 Add Comments State to Playback Controller

Extend `playback_controller.h` to add a new playback source:

```c
typedef enum {
    PLAYBACK_SOURCE_NONE,
    PLAYBACK_SOURCE_PICO8_STREAM,
    PLAYBACK_SOURCE_LOCAL_ANIMATION,
    PLAYBACK_SOURCE_COMMENTS       // NEW: Artwork comments mode
} playback_source_t;
```

Add functions to enter/exit comments mode:
```c
esp_err_t playback_controller_enter_comments_mode(void);
void playback_controller_exit_comments_mode(void);
bool playback_controller_is_comments_active(void);
```

#### 1.3 Update Display Renderer Modes

Add new render mode to `display_renderer.h`:

```c
typedef enum {
    DISPLAY_RENDER_MODE_ANIMATION,
    DISPLAY_RENDER_MODE_UI,
    DISPLAY_RENDER_MODE_COMMENTS    // NEW: Comments rendering mode
} display_render_mode_t;
```

Add functions:
```c
esp_err_t display_renderer_enter_comments_mode(void);
void display_renderer_exit_comments_mode(void);
bool display_renderer_is_comments_mode(void);
```

### Phase 2: Touch Gesture Detection

#### 2.1 Add 2-Finger Vertical Swipe Detection

In `app_touch.c`, add new gesture state:

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

Detection logic (in `app_touch_task`):
- Detect when 2 fingers are touching (already have multi-touch support)
- Track vertical movement of both fingers
- If both fingers move in same direction (up or down) by minimum threshold
- And horizontal movement is small
- Trigger comments mode entry/exit

**Swipe direction mapping**:
- **2-finger swipe UP**: Enter comments mode
- **2-finger swipe DOWN** (when in comments): Exit comments mode
- **1-finger swipe UP/DOWN** (when in comments): Scroll comments

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

In `display_renderer.c`, add new mode handling:

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
        // Set up shared state for workers
        g_comments_dst_buffer = back_buffer;
        g_comments_page = page;
        g_comments_viewport_y = page->scroll_offset;
        
        // Split screen between workers (like upscale)
        const int mid_row = EXAMPLE_LCD_V_RES / 2;
        g_comments_row_start_top = 0;
        g_comments_row_end_top = mid_row;
        g_comments_row_start_bottom = mid_row;
        g_comments_row_end_bottom = EXAMPLE_LCD_V_RES;
        
        // Notify workers and wait for completion
        DISPLAY_MEMORY_BARRIER();
        xTaskNotify(g_upscale_worker_top, 1, eSetBits);
        xTaskNotify(g_upscale_worker_bottom, 1, eSetBits);
        
        // Wait for both workers (use notification bits)
        // ... (same pattern as upscaling)
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

Add to `components/makapix/makapix_mqtt.h`:

```c
// Request comments for current artwork
// Publish to: makapix/device/{device_id}/artwork/comments/request
// Payload: {"artwork_id": "<id>"}

// Receive comments response
// Subscribe to: makapix/device/{device_id}/artwork/comments/response
// Payload: {
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
```

#### 7.2 MQTT Handler Implementation

Create `components/makapix/makapix_comments.c`:

```c
esp_err_t makapix_request_comments(const char *artwork_id);
esp_err_t makapix_parse_comments_response(const char *json_payload);
```

Parser must:
1. Parse JSON response
2. Build tree structure of comments
3. Enforce max depth of 2
4. Allocate memory for all strings
5. Call `layout_comments_page()` after building tree

### Phase 8: Main State Machine Integration

#### 8.1 State Transition Logic

In `p3a_main.c`, add comments monitoring similar to provisioning:

```c
static void comments_monitor_task(void *arg) {
    while (true) {
        if (artwork_comments_is_active()) {
            // Comments mode is active
            // Check for exit condition or updates
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```

#### 8.2 Touch Handler Integration

In `app_touch.c`, add handler for entering comments:

```c
if (/* 2-finger vertical swipe detected */) {
    if (!artwork_comments_is_active()) {
        // Enter comments mode
        ESP_LOGI(TAG, "Entering artwork comments mode");
        artwork_comments_enter();
    } else {
        // Exit comments mode
        ESP_LOGI(TAG, "Exiting artwork comments mode");
        artwork_comments_exit();
    }
}
```

When in comments mode, remap 1-finger vertical swipes to scrolling:

```c
if (artwork_comments_is_active() && gesture_state == GESTURE_STATE_BRIGHTNESS) {
    // Repurpose brightness gesture as scrolling
    int16_t scroll_delta = delta_y;  // Y movement
    artwork_comments_scroll(scroll_delta);
    gesture_state = GESTURE_STATE_SCROLLING;
}
```

### Phase 9: Configuration System

#### 9.1 Runtime Configuration

Add to `components/config_store/`:

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
- [ ] Create data structures (`artwork_comments.h`)
- [ ] Implement basic state management (`artwork_comments.c`)
- [ ] Add render mode to display renderer
- [ ] Update playback controller

### Step 2: Touch Input (Day 3)
- [ ] Implement 2-finger vertical swipe detection
- [ ] Add gesture state transitions
- [ ] Test gesture recognition

### Step 3: Rendering Core (Days 4-5)
- [ ] Create sprite system
- [ ] Implement comment box drawing
- [ ] Add text rendering and wrapping
- [ ] Test individual component rendering

### Step 4: Layout Engine (Days 6-7)
- [ ] Implement layout algorithm
- [ ] Handle nested comments correctly
- [ ] Calculate page height
- [ ] Test layout with mock data

### Step 5: Worker Integration (Days 8-9)
- [ ] Extend worker tasks for UI rendering
- [ ] Implement viewport clipping
- [ ] Optimize drawing performance
- [ ] Test split-screen rendering

### Step 6: MQTT Integration (Days 10-11)
- [ ] Define MQTT protocol
- [ ] Implement request/response handlers
- [ ] Add JSON parsing
- [ ] Test with mock server

### Step 7: Main Loop Integration (Days 12-13)
- [ ] Wire up state transitions
- [ ] Connect touch gestures to comments mode
- [ ] Implement scrolling
- [ ] Test end-to-end flow

### Step 8: Polish (Days 14-15)
- [ ] Add loading indicators
- [ ] Handle error cases (no comments, network failure)
- [ ] Performance optimization
- [ ] Memory leak testing

## Testing Strategy

### Unit Tests
- Layout algorithm with various comment depths
- Comment box rendering at different sizes
- Viewport clipping edge cases
- JSON parsing for malformed data

### Integration Tests
- Enter/exit comments mode
- Scrolling behavior
- MQTT request/response cycle
- State transitions with other modes (provisioning, OTA)

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
   - **Recommendation**: Show "No comments yet" message in center

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

- Existing provisioning UI: `main/ugfx_ui.c`
- Touch gesture system: `main/app_touch.c`
- Display renderer architecture: `main/display_renderer.c`
- MQTT implementation: `components/makapix/makapix_mqtt.c`
- Worker task pattern: `display_upscale_worker_*_task` functions

## Appendix A: File Structure

New files to create:
```
main/
  include/
    artwork_comments.h           - Public API
    comments_render.h            - Rendering API
    comments_layout.h            - Layout API
  artwork_comments.c             - Core state management
  comments_render.c              - UI rendering implementation
  comments_layout.c              - Layout algorithm
  comments_sprites.c             - Sprite data and drawing

components/makapix/
  makapix_comments.c             - MQTT comment fetching
  makapix_comments.h             - MQTT comment API

components/config_store/
  - Add color configuration APIs
```

Modified files:
```
main/
  app_touch.c                    - Add 2-finger swipe detection
  display_renderer.c             - Add comments render mode
  display_renderer_priv.h        - Add comments worker state
  playback_controller.c          - Add comments playback source
  p3a_main.c                     - Add comments state monitoring

main/include/
  display_renderer.h             - Add comments mode APIs
  playback_controller.h          - Add comments mode APIs
```

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
