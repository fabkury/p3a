# Screen Rotation Implementation Guide

> **Status**: ✅ **IMPLEMENTED** — Screen rotation is fully functional as of the latest firmware release.

## Executive Summary

This document describes the implementation of screen rotation (0°, 90°, 180°, 270°) on the p3a pixel art player. The device uses a 720×720 square IPS display with an ESP32-P4 microcontroller running efficient multi-buffered animation playback.

The solution coordinates rotation across two rendering subsystems:
1. **Animation Player**: Runtime coordinate transformation in the blit function (minimal overhead)
2. **µGFX UI**: Native driver-level rotation support via `GDISP_NEED_CONTROL`

Both subsystems are unified through a single API (`app_set_screen_rotation()`) for consistent screen orientation across all display modes. Rotation can be controlled via:
- **Two-finger touch gesture** (rotate fingers clockwise/counter-clockwise)
- **REST API** (`GET/POST /api/rotation`)
- **Web interface** (rotation control in settings)

Rotation settings are persisted to NVS and restored on boot.

---

## Current Architecture Overview

### Display Pipeline

The p3a uses a sophisticated display pipeline optimized for pixel art animation playback:

1. **Resolution**: 720×720 pixels (square display)
2. **Buffer Configuration**: 2-3 frame buffers for smooth vsync rendering
3. **Rendering Flow**:
   - Decoder (WebP/GIF/PNG/JPEG) → Native frame buffer (source resolution)
   - Upscale with lookup tables → LCD frame buffer (720×720)
   - DPI panel → Display

### Key Components

- **`animation_player_render.c`**: Core rendering loop (`lcd_animation_task`)
- **`blit_upscaled_rows()`**: Upscales decoded RGBA frames using pre-computed lookup tables
- **Upscale workers**: Two parallel tasks split rendering (top/bottom halves)
- **Frame buffers**: Direct MIPI-DSI DPI panel buffers
- **LCD driver**: `esp_lcd_mipi_dsi` with `esp_lcd_dpi_panel`

### Current Upscaling Method

The system uses pre-computed X/Y lookup tables for nearest-neighbor upscaling:
```c
// Generated once per animation load
buf->upscale_lookup_x[dst_x] = (dst_x * canvas_w) / target_w;
buf->upscale_lookup_y[dst_y] = (dst_y * canvas_h) / target_h;
```

Rendering copies source pixels row-by-row:
```c
for (int dst_y = row_start; dst_y < row_end; ++dst_y) {
    const uint16_t src_y = lookup_y[dst_y];
    const uint8_t *src_row = src_rgba + (size_t)src_y * src_w * 4;
    
    for (int dst_x = 0; dst_x < dst_w; ++dst_x) {
        const uint16_t src_x = lookup_x[dst_x];
        const uint8_t *pixel = src_row + (size_t)src_x * 4;
        dst_row[dst_x] = rgb565(pixel[0], pixel[1], pixel[2]);
    }
}
```

---

## Unified Rotation System

### Overview

The implementation coordinates rotation across both animation playback and UI rendering through a unified API. Each subsystem handles rotation using its optimal approach:

- **Animation Player**: Encode rotation in upscale lookup tables (zero runtime overhead)
- **µGFX UI**: Use native driver rotation (automatic, transparent)

### Architecture Diagram

```
┌─────────────────────────────────────────────────┐
│     app_set_screen_rotation(angle)              │
│            (Unified API)                         │
└─────────────┬───────────────────────────────────┘
              │
      ┌───────┴────────┐
      │                │
┌─────▼─────┐   ┌─────▼──────┐
│ Animation │   │  µGFX UI   │
│  Rotation │   │  Rotation  │
└─────┬─────┘   └─────┬──────┘
      │               │
      │               │
┌─────▼─────┐   ┌─────▼──────┐
│  Lookup   │   │   Driver   │
│  Tables   │   │  Rotation  │
└───────────┘   └────────────┘
```

---

## Animation Player Rotation

### Implementation: Lookup Table Transformation

**How It Works**: Rotation is encoded into the upscale lookup tables during animation load. The rendering loop remains unchanged—rotation is "baked in" to the coordinate mapping.

### Rotation Transformations

```c
// Identity (0°): (x, y) → (x, y)
for (int dst_x = 0; dst_x < target_w; ++dst_x) {
    buf->upscale_lookup_x[dst_x] = (dst_x * canvas_w) / target_w;
}
for (int dst_y = 0; dst_y < target_h; ++dst_y) {
    buf->upscale_lookup_y[dst_y] = (dst_y * canvas_h) / target_h;
}

// 90° CW: (x, y) → (y, width - 1 - x)
for (int dst_x = 0; dst_x < target_w; ++dst_x) {
    int src_coord = dst_x; // Maps to source Y
    buf->upscale_lookup_x[dst_x] = (src_coord * canvas_h) / target_w;
}
for (int dst_y = 0; dst_y < target_h; ++dst_y) {
    int src_coord = target_w - 1 - dst_y; // Maps to (width - 1 - source X)
    buf->upscale_lookup_y[dst_y] = (src_coord * canvas_w) / target_h;
}

// 180°: (x, y) → (width - 1 - x, height - 1 - y)
for (int dst_x = 0; dst_x < target_w; ++dst_x) {
    int src_coord = target_w - 1 - dst_x;
    buf->upscale_lookup_x[dst_x] = (src_coord * canvas_w) / target_w;
}
for (int dst_y = 0; dst_y < target_h; ++dst_y) {
    int src_coord = target_h - 1 - dst_y;
    buf->upscale_lookup_y[dst_y] = (src_coord * canvas_h) / target_h;
}

// 270° CW: (x, y) → (height - 1 - y, x)
for (int dst_x = 0; dst_x < target_w; ++dst_x) {
    int src_coord = target_h - 1 - dst_x; // Maps to (height - 1 - source Y)
    buf->upscale_lookup_x[dst_x] = (src_coord * canvas_h) / target_w;
}
for (int dst_y = 0; dst_y < target_h; ++dst_y) {
    int src_coord = dst_y; // Maps to source X
    buf->upscale_lookup_y[dst_y] = (src_coord * canvas_w) / target_h;
}
```

### Changes Required

**File: `main/animation_player_loader.c`** (~50 lines)

Modify the lookup table generation function (around line 405):

```c
static esp_err_t generate_upscale_lookup_tables(
    animation_buffer_t *buf, 
    int canvas_w, 
    int canvas_h,
    screen_rotation_t rotation)
{
    const int target_w = EXAMPLE_LCD_H_RES;
    const int target_h = EXAMPLE_LCD_V_RES;
    
    // Allocate lookup tables
    heap_caps_free(buf->upscale_lookup_x);
    heap_caps_free(buf->upscale_lookup_y);
    
    buf->upscale_lookup_x = (uint16_t *)heap_caps_malloc(
        (size_t)target_w * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
    if (!buf->upscale_lookup_x) {
        return ESP_ERR_NO_MEM;
    }
    
    buf->upscale_lookup_y = (uint16_t *)heap_caps_malloc(
        (size_t)target_h * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
    if (!buf->upscale_lookup_y) {
        heap_caps_free(buf->upscale_lookup_x);
        buf->upscale_lookup_x = NULL;
        return ESP_ERR_NO_MEM;
    }
    
    // Generate lookup tables based on rotation
    switch (rotation) {
        case ROTATION_0:
            // Standard mapping
            for (int dst_x = 0; dst_x < target_w; ++dst_x) {
                int src_x = (dst_x * canvas_w) / target_w;
                if (src_x >= canvas_w) src_x = canvas_w - 1;
                buf->upscale_lookup_x[dst_x] = (uint16_t)src_x;
            }
            for (int dst_y = 0; dst_y < target_h; ++dst_y) {
                int src_y = (dst_y * canvas_h) / target_h;
                if (src_y >= canvas_h) src_y = canvas_h - 1;
                buf->upscale_lookup_y[dst_y] = (uint16_t)src_y;
            }
            break;
            
        case ROTATION_90:
            // 90° CW: (x,y) → (y, width-1-x)
            for (int dst_x = 0; dst_x < target_w; ++dst_x) {
                int src_y = (dst_x * canvas_h) / target_w;
                if (src_y >= canvas_h) src_y = canvas_h - 1;
                buf->upscale_lookup_x[dst_x] = (uint16_t)src_y;
            }
            for (int dst_y = 0; dst_y < target_h; ++dst_y) {
                int src_x = ((target_w - 1 - dst_y) * canvas_w) / target_h;
                if (src_x < 0) src_x = 0;
                if (src_x >= canvas_w) src_x = canvas_w - 1;
                buf->upscale_lookup_y[dst_y] = (uint16_t)src_x;
            }
            break;
            
        case ROTATION_180:
            // 180°: (x,y) → (width-1-x, height-1-y)
            for (int dst_x = 0; dst_x < target_w; ++dst_x) {
                int src_x = ((target_w - 1 - dst_x) * canvas_w) / target_w;
                if (src_x < 0) src_x = 0;
                if (src_x >= canvas_w) src_x = canvas_w - 1;
                buf->upscale_lookup_x[dst_x] = (uint16_t)src_x;
            }
            for (int dst_y = 0; dst_y < target_h; ++dst_y) {
                int src_y = ((target_h - 1 - dst_y) * canvas_h) / target_h;
                if (src_y < 0) src_y = 0;
                if (src_y >= canvas_h) src_y = canvas_h - 1;
                buf->upscale_lookup_y[dst_y] = (uint16_t)src_y;
            }
            break;
            
        case ROTATION_270:
            // 270° CW: (x,y) → (height-1-y, x)
            for (int dst_x = 0; dst_x < target_w; ++dst_x) {
                int src_y = ((target_h - 1 - dst_x) * canvas_h) / target_w;
                if (src_y < 0) src_y = 0;
                if (src_y >= canvas_h) src_y = canvas_h - 1;
                buf->upscale_lookup_x[dst_x] = (uint16_t)src_y;
            }
            for (int dst_y = 0; dst_y < target_h; ++dst_y) {
                int src_x = (dst_y * canvas_w) / target_h;
                if (src_x >= canvas_w) src_x = canvas_w - 1;
                buf->upscale_lookup_y[dst_y] = (uint16_t)src_x;
            }
            break;
    }
    
    buf->upscale_src_w = canvas_w;
    buf->upscale_src_h = canvas_h;
    buf->upscale_dst_w = target_w;
    buf->upscale_dst_h = target_h;
    
    return ESP_OK;
}
```

### Performance Characteristics

- **Lookup table generation**: ~0.1ms one-time cost per animation load
- **Per-frame rendering**: 0% overhead (same memory access pattern)
- **CPU usage**: Unchanged
- **Memory**: No additional allocation (uses existing lookup tables)

### Runtime Rotation

When rotation changes:
1. Trigger back buffer reload via loader task
2. Next animation load generates new lookup tables
3. Buffer swap happens smoothly (1-2 frames)
4. Current animation finishes at old rotation

---

## µGFX UI Rotation

### Overview

µGFX has native rotation support built into the framebuffer driver. **All rendering operations automatically respect the current orientation** without any application-level transformations needed.

### How µGFX Rotation Works

**Key Insight**: Rotation is handled transparently at the driver level. When you call high-level functions like `gdispFillStringBox()`, the text rendering engine calls the low-level `gdisp_lld_draw_pixel()` function for each pixel. This driver function applies coordinate transformation based on the current orientation before writing to the framebuffer.

**Architecture**:
```
Application Code
    ↓
gdispFillStringBox() / gdispClear() / etc.
    ↓
µGFX Graphics Engine (text/shape rendering)
    ↓
gdisp_lld_draw_pixel() [ROTATION HAPPENS HERE]
    ↓
Framebuffer Write
```

**Driver Implementation** (`components/ugfx/drivers/gdisp/framebuffer/gdisp_lld_framebuffer.c`):

```c
LLDSPEC void gdisp_lld_draw_pixel(GDisplay *g) {
    unsigned pos;
    
    // Coordinate transformation based on orientation
    switch(g->g.Orientation) {
    case gOrientation0:   // 0° - No transformation
        pos = PIXIL_POS(g, g->p.x, g->p.y);
        break;
    case gOrientation90:  // 90° CW
        pos = PIXIL_POS(g, g->p.y, g->g.Width-g->p.x-1);
        break;
    case gOrientation180: // 180°
        pos = PIXIL_POS(g, g->g.Width-g->p.x-1, g->g.Height-g->p.y-1);
        break;
    case gOrientation270: // 270° CW
        pos = PIXIL_POS(g, g->g.Height-g->p.y-1, g->p.x);
        break;
    }
    
    // Write pixel to framebuffer
    LLDCOLOR_TYPE native = gdispColor2Native(g->p.color);
    memcpy(PIXEL_ADDR(g, pos), &native, FB_BYTES_PER_PIXEL);
}
```

### Why This Is Not "Per-Pixel Postprocessing"

The coordinate transformation is **not** a separate post-processing step. It's an **integral part of the rendering process**:

1. **No extra memory**: No intermediate buffers or copies
2. **No extra pass**: Transformation happens during the write operation itself
3. **Native rendering**: Text and shapes are rendered directly at the rotated orientation
4. **Automatic**: All µGFX drawing operations benefit automatically

Think of it like this: Instead of rendering to coordinates (x, y) and then rotating, µGFX calculates rotated coordinates and renders directly there. It's native rendering at a rotated orientation.

### Width/Height Handling

µGFX automatically swaps display dimensions when switching between portrait/landscape:

```c
case GDISP_CONTROL_ORIENTATION:
    switch((gOrientation)g->p.ptr) {
        case gOrientation0:
        case gOrientation180:
            // Portrait/landscape preserved
            if (g->g.Orientation == gOrientation90 || 
                g->g.Orientation == gOrientation270) {
                // Swap back from rotated
                gCoord tmp = g->g.Width;
                g->g.Width = g->g.Height;
                g->g.Height = tmp;
            }
            break;
        case gOrientation90:
        case gOrientation270:
            // Need to swap for rotation
            if (g->g.Orientation == gOrientation0 || 
                g->g.Orientation == gOrientation180) {
                gCoord tmp = g->g.Width;
                g->g.Width = g->g.Height;
                g->g.Height = tmp;
            }
            break;
    }
    g->g.Orientation = (gOrientation)g->p.ptr;
```

For a 720×720 square display, width and height remain equal, so this is a no-op in practice.

### Changes Required

**File: `main/ugfx_ui.c`** (~30 lines)

Add rotation synchronization function:

```c
esp_err_t ugfx_ui_set_rotation(screen_rotation_t rotation) {
    gOrientation ugfx_orientation;
    
    // Map screen_rotation_t to µGFX gOrientation
    switch (rotation) {
        case ROTATION_0:
            ugfx_orientation = gOrientation0;
            break;
        case ROTATION_90:
            ugfx_orientation = gOrientation90;
            break;
        case ROTATION_180:
            ugfx_orientation = gOrientation180;
            break;
        case ROTATION_270:
            ugfx_orientation = gOrientation270;
            break;
        default:
            ESP_LOGE(TAG, "Invalid rotation angle: %d", rotation);
            return ESP_ERR_INVALID_ARG;
    }
    
    // Apply orientation if µGFX is initialized
    if (s_ugfx_initialized) {
        gdispSetOrientation(ugfx_orientation);
        ESP_LOGI(TAG, "µGFX orientation set to %d degrees", rotation);
    }
    
    return ESP_OK;
}
```

**File: `main/ugfx_ui.h`** (~5 lines)

Expose the function:

```c
/**
 * @brief Set µGFX UI rotation
 * 
 * Sets the display orientation for µGFX rendering. Must be called after
 * µGFX is initialized. Takes effect immediately.
 * 
 * @param rotation Screen rotation angle
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ugfx_ui_set_rotation(screen_rotation_t rotation);
```

### Performance Characteristics

- **Per-pixel overhead**: ~10-15 CPU cycles (switch + arithmetic)
- **Registration UI frame**: ~50,000 pixels rendered (text only)
- **Total overhead**: ~0.5ms per frame
- **Update frequency**: 1 Hz (countdown timer)
- **Impact**: Negligible - UI is not latency-sensitive

**Why This Is Acceptable**:
1. UI renders at **1 FPS** (once per second for timer updates)
2. UI is **low complexity** (text rendering only, no graphics)
3. UI has **no strict timing** requirements (registration/provisioning screen)
4. ESP32-P4 dual-core easily handles the overhead

---

## Touch Coordinate Transformation

### Implementation

Touch input coordinates must be transformed to match the rotated display.

**File: `main/app_touch.c`** (~20 lines)

```c
/**
 * @brief Transform touch coordinates based on current screen rotation
 * 
 * @param x Pointer to X coordinate (modified in place)
 * @param y Pointer to Y coordinate (modified in place)
 * @param rotation Current screen rotation
 */
static void transform_touch_coordinates(int *x, int *y, screen_rotation_t rotation) {
    const int screen_w = EXAMPLE_LCD_H_RES;
    const int screen_h = EXAMPLE_LCD_V_RES;
    int temp;
    
    switch (rotation) {
        case ROTATION_0:
            // No transformation
            break;
            
        case ROTATION_90:  // 90° CW
            // (x, y) → (y, height - 1 - x)
            temp = *x;
            *x = *y;
            *y = screen_h - 1 - temp;
            break;
            
        case ROTATION_180:
            // (x, y) → (width - 1 - x, height - 1 - y)
            *x = screen_w - 1 - *x;
            *y = screen_h - 1 - *y;
            break;
            
        case ROTATION_270:  // 270° CW (90° CCW)
            // (x, y) → (width - 1 - y, x)
            temp = *x;
            *x = screen_w - 1 - *y;
            *y = temp;
            break;
    }
}
```

Apply transformation before gesture processing in the touch event handler.

---

## Unified API

### Global Rotation State

**File: `main/animation_player_priv.h`**

```c
typedef enum {
    ROTATION_0   = 0,
    ROTATION_90  = 90,
    ROTATION_180 = 180,
    ROTATION_270 = 270
} screen_rotation_t;

extern screen_rotation_t g_screen_rotation;
```

### Public API

**File: `main/animation_player.h`**

```c
/**
 * @brief Set screen rotation for entire display
 * 
 * Applies rotation to both animation playback and UI rendering.
 * Animation rotation takes effect on next animation load (1-2 frames).
 * UI rotation takes effect immediately.
 * 
 * @param rotation Rotation angle (0°, 90°, 180°, 270°)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t app_set_screen_rotation(screen_rotation_t rotation);

/**
 * @brief Get current screen rotation
 * 
 * @return Current rotation angle
 */
screen_rotation_t app_get_screen_rotation(void);
```

### Implementation

**File: `main/animation_player.c`** (~40 lines)

```c
screen_rotation_t g_screen_rotation = ROTATION_0;

esp_err_t app_set_screen_rotation(screen_rotation_t rotation) {
    // Validate rotation angle
    if (rotation != ROTATION_0 && rotation != ROTATION_90 && 
        rotation != ROTATION_180 && rotation != ROTATION_270) {
        ESP_LOGE(TAG, "Invalid rotation angle: %d", rotation);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Update global state
    g_screen_rotation = rotation;
    
    // Apply to µGFX UI immediately
    esp_err_t err = ugfx_ui_set_rotation(rotation);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set µGFX rotation: %s", esp_err_to_name(err));
        // Continue anyway - UI rotation is non-critical
    }
    
    // Trigger animation reload with new rotation
    // The loader task will regenerate lookup tables
    if (s_buffer_mutex && xSemaphoreTake(s_buffer_mutex, portMAX_DELAY) == pdTRUE) {
        s_swap_requested = true;
        xSemaphoreGive(s_buffer_mutex);
        xSemaphoreGive(s_loader_sem);
    }
    
    // Store in config for persistence
    config_store_set_rotation(rotation);
    
    ESP_LOGI(TAG, "Screen rotation set to %d degrees", rotation);
    return ESP_OK;
}

screen_rotation_t app_get_screen_rotation(void) {
    return g_screen_rotation;
}
```

### Initialization

On system startup, load rotation from config and apply:

```c
esp_err_t animation_player_init(...) {
    // ... existing init code ...
    
    // Load and apply saved rotation
    screen_rotation_t saved_rotation = config_store_get_rotation();
    if (saved_rotation != ROTATION_0) {
        app_set_screen_rotation(saved_rotation);
    }
    
    // ... rest of init ...
}
```

---

## Configuration Storage

### NVS Persistence

**File: `components/config_store/config_store.c`** (~30 lines)

```c
#define CONFIG_KEY_ROTATION "rotation"

esp_err_t config_store_set_rotation(screen_rotation_t rotation) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    
    err = nvs_set_u8(handle, CONFIG_KEY_ROTATION, (uint8_t)rotation);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    
    nvs_close(handle);
    return err;
}

screen_rotation_t config_store_get_rotation(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return ROTATION_0; // Default
    }
    
    uint8_t value = 0;
    err = nvs_get_u8(handle, CONFIG_KEY_ROTATION, &value);
    nvs_close(handle);
    
    if (err != ESP_OK) {
        return ROTATION_0; // Default
    }
    
    return (screen_rotation_t)value;
}
```

---

## Web API

### REST Endpoint

**File: `components/http_api/http_api.c`** (~40 lines)

```c
/**
 * GET /api/rotation
 * Returns: {"rotation": 90}
 */
static esp_err_t get_rotation_handler(httpd_req_t *req) {
    screen_rotation_t rotation = app_get_screen_rotation();
    
    char json[64];
    snprintf(json, sizeof(json), "{\"rotation\":%d}", (int)rotation);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

/**
 * POST /api/rotation
 * Body: {"rotation": 90}
 */
static esp_err_t set_rotation_handler(httpd_req_t *req) {
    char content[64] = {0};
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
        return ESP_FAIL;
    }
    
    // Parse JSON
    int rotation_value = 0;
    if (sscanf(content, "{\"rotation\":%d}", &rotation_value) != 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    // Validate and apply
    esp_err_t err = app_set_screen_rotation((screen_rotation_t)rotation_value);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid rotation angle");
        return ESP_FAIL;
    }
    
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}
```

---

## Implementation Checklist

### Phase 1: Core Infrastructure
- [x] Define `screen_rotation_t` enum in `animation_player.h` (shared header)
- [x] Add global rotation state variable (`g_screen_rotation`)
- [x] Implement touch coordinate transformation in `app_touch.c`
- [x] Add rotation config functions in `config_store` component

### Phase 2: Animation Player
- [x] Simplified lookup table generation (standard upscale only)
- [x] Implement coordinate transformations in `blit_upscaled_rows()` for all 4 rotation angles
- [x] Add `s_upscale_rotation` variable to track rotation state
- [x] Modify render functions to set rotation state before upscaling
- [x] Test with sample animations at each rotation angle

### Phase 3: µGFX UI Integration
- [x] Enable `GDISP_NEED_CONTROL` in `gfxconf.h`
- [x] Add `ugfx_ui_set_rotation()` function in `ugfx_ui.c`
- [x] Expose function in `ugfx_ui.h`
- [x] Add pending orientation state for initialization timing
- [x] Test registration screen at each rotation angle

### Phase 4: Unified API
- [x] Implement `app_set_screen_rotation()` in `animation_player.c`
- [x] Implement `app_get_screen_rotation()` in `animation_player.c`
- [x] Add initialization code to load saved rotation on boot
- [x] Test rotation changes during runtime

### Phase 5: Web Interface
- [x] Add `GET /api/rotation` endpoint
- [x] Add `POST /api/rotation` endpoint
- [x] Web UI can be updated to include rotation control (API ready)

### Phase 6: Touch Gesture
- [x] Implement two-finger rotation gesture detection
- [x] Calculate angle between two touch points
- [x] Track cumulative rotation angle
- [x] Trigger screen rotation when threshold exceeded (~45°)
- [x] Fix gesture direction mapping (clockwise/counter-clockwise)

### Phase 7: Integration Testing
- [x] Test all 4 rotation angles in animation mode
- [x] Test all 4 rotation angles in UI mode
- [x] Test switching between animation and UI modes with rotation
- [x] Test rotation changes while animation is playing
- [x] Test rotation changes while in UI mode
- [x] Verify touch input works correctly at all rotations
- [x] Verify rotation persists across reboots
- [x] Performance validation (minimal overhead in blit function)

---

## Technical Specifications

### Memory Requirements
- Lookup tables: 2 × 720 × 2 bytes = 2.8KB (already allocated)
- µGFX rotation: 0 bytes (coordinate transformation only)
- Config storage: 4 bytes (rotation value in NVS)
- **Total additional: 4 bytes**

### Performance Targets
- **Animation rendering**: 0ms per-frame overhead
- **UI rendering**: ~0.5ms per-frame overhead (at 1 FPS)
- **Rotation switch time**: < 100ms
- **Animation load time increase**: < 0.1ms
- **Frame rate**: Unchanged (60 FPS capable)

### Compatibility Matrix

| Feature | 0° | 90° | 180° | 270° | Notes |
|---------|----|----|-----|------|-------|
| WebP Animation | ✓ | ✓ | ✓ | ✓ | Lookup table rotation |
| GIF Animation | ✓ | ✓ | ✓ | ✓ | Lookup table rotation |
| PNG Static | ✓ | ✓ | ✓ | ✓ | Lookup table rotation |
| JPEG Static | ✓ | ✓ | ✓ | ✓ | Lookup table rotation |
| UI (µGFX) | ✓ | ✓ | ✓ | ✓ | Native driver rotation |
| Touch Input | ✓ | ✓ | ✓ | ✓ | Coordinate transformation |
| PICO-8 Stream | ✓ | ⚠️ | ⚠️ | ⚠️ | Future work |

⚠️ = PICO-8 stream requires separate implementation (out of scope for initial version)

---

## Code Locations Summary

### Files to Modify

1. **`main/animation_player_priv.h`**
   - Add `screen_rotation_t` enum
   - Add global rotation state variable

2. **`main/animation_player_loader.c`**
   - Refactor lookup table generation (~50 lines)
   - Add rotation parameter handling

3. **`main/animation_player.c`**
   - Implement unified API (~40 lines)
   - Add initialization code

4. **`main/animation_player.h`**
   - Public API declarations

5. **`main/ugfx_ui.c`**
   - Add `ugfx_ui_set_rotation()` (~30 lines)

6. **`main/ugfx_ui.h`**
   - Function declaration

7. **`main/app_touch.c`**
   - Touch coordinate transformation (~20 lines)

8. **`components/config_store/config_store.c`**
   - NVS storage functions (~30 lines)

9. **`components/config_store/config_store.h`**
   - Function declarations

10. **`components/http_api/http_api.c`**
    - REST API endpoints (~40 lines)

### Estimated Total LOC: ~150 lines
- Animation rotation: ~90 lines
- µGFX integration: ~35 lines
- Touch transformation: ~20 lines
- Config/API: ~30 lines (excluding web UI HTML/JS)

---

## Conclusion

The unified rotation system provides comprehensive screen rotation support by leveraging the optimal approach for each rendering subsystem:

### Key Benefits

1. **Zero overhead for animations**: Lookup table approach preserves accurate frame timing
2. **Native µGFX support**: Driver-level rotation requires no application-level transformations
3. **Unified API**: Single function call to rotate entire display
4. **Consistent UX**: Same orientation across all display modes
5. **Minimal implementation**: ~150 lines of code
6. **Memory efficient**: No additional buffers required
7. **Runtime flexible**: Can change rotation during operation

### Why This Solution Works

- **For animations** (primary use case): Zero overhead maintains 60 FPS capability critical for accurate pixel art playback
- **For UI** (provisioning): Native driver rotation with negligible overhead acceptable at 1 FPS update rate
- **System integration**: Touch coordinates transformed consistently, single rotation state across all modes

This implementation aligns perfectly with p3a's core function as a pixel art player where frame timing accuracy is paramount, while providing full rotation support for the UI without compromising performance.

---

## Implementation Notes

### Actual Implementation Approach

The final implementation differs slightly from the original plan:

**Animation Rotation**: Instead of encoding rotation into lookup tables (which doesn't work for 90°/270° rotations), rotation is handled at render time in `blit_upscaled_rows()`. The lookup tables remain standard upscale tables, and rotation transformations are applied during pixel copying. This approach:
- Works correctly for all rotation angles (including 90°/270°)
- Maintains compatibility with the parallel upscale workers (top/bottom split)
- Has minimal performance overhead (~5-10% for rotated frames)
- Simplifies code maintenance

**Touch Gestures**: 
- Two-finger rotation gesture detects finger angle changes
- Gestures use raw (physical) coordinates for movement detection
- Only tap position is transformed for left/right half detection
- Rotation threshold: ~45° finger rotation triggers 90° screen rotation

**µGFX Integration**:
- `GDISP_NEED_CONTROL` enabled in `gfxconf.h`
- Pending orientation state handles rotation set before µGFX initialization
- Rotation persists across UI mode switches

All rotation settings are persisted to NVS and restored on device boot.

### Next Steps

1. Implement core infrastructure (rotation enum, config storage)
2. Add animation player rotation (lookup table generation)
3. Integrate µGFX rotation (simple API call)
4. Implement touch coordinate transformation
5. Add unified API and web interface
6. Test thoroughly across all display modes and rotations
