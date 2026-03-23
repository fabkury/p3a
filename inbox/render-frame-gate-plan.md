# Plan: Render Frame Gate for Stutter-Free Animation Playback

## Context

WebP animations stutter during Giphy channel refreshes, while GIF animations play smoothly. The root cause is **L2 cache contention** between the render pipeline (Core 1, priority 8) and the Giphy refresh task (Core 0, priority 4).

The ESP32-P4's 256KB L2 cache is shared between both cores. During a Giphy refresh, heavy PSRAM operations pollute the cache:
- HTTP response receive: ~94KB written to PSRAM buffer per page (6 pages typical)
- cJSON parsing: hundreds of heap-allocated nodes from ~94KB JSON
- Hash table merging/eviction: random PSRAM pointer chasing

These evict cache lines that the WebP decoder needs, pushing decode time past the VSYNC deadline (~16.6ms at 60Hz). Frames that miss their VSYNC window display for 33ms instead of 16.6ms = visible stutter.

**Why WebP and not GIF**: WebP decode (`WebPAnimDecoderGetNext`) has a large working set (~700KB+: compressed data + VP8L decode state + full RGBA output) that already exceeds the L2 cache. Any additional cache pressure from concurrent PSRAM access causes cache thrashing. GIF decode uses incremental palette-based updates with a small working set (~1KB palette + only changed rectangles), which is cache-resilient.

## Approach: Render Frame Gate

A cooperative yielding mechanism where the render task signals when it's actively decoding a frame, and network/refresh tasks check this signal and yield during their cache-heavy operations.

This follows the existing `animation_player_is_sd_paused()` pattern already used by `download_manager.c` and `makapix_channel_impl.c` (weak extern symbols from `main/`).

## Changes

### 1. Display renderer: add gate flag and wrap frame callback

**`main/display_renderer.c`** (~line 30, add flag):
```c
static volatile bool g_frame_rendering_active = false;
```

Add three functions:
```c
void display_renderer_frame_gate_enter(void) { g_frame_rendering_active = true; }
void display_renderer_frame_gate_leave(void) { g_frame_rendering_active = false; }
bool display_renderer_is_frame_rendering(void) { return g_frame_rendering_active; }
```

Wrap the frame callback (around lines 554-556):
```c
display_renderer_frame_gate_enter();
frame_delay_ms = callback(back_buffer, ctx);
display_renderer_frame_gate_leave();
```

Only the non-UI animation callback path needs gating (the `else` branch at line 550, not the `ui_mode` branch).

**`main/include/display_renderer.h`**: Add declarations for the three gate functions.

### 2. Giphy API: yield during HTTP receive and before JSON parse

**`components/giphy/giphy_api.c`**

Add weak extern + yield helper at top:
```c
extern bool display_renderer_is_frame_rendering(void) __attribute__((weak));

static inline void yield_for_frame_gate(void) {
    if (display_renderer_is_frame_rendering && display_renderer_is_frame_rendering()) {
        vTaskDelay(1);
    }
}
```

Insert yield points:
- **After each `esp_http_client_read()` in the receive loop** (line ~340): `yield_for_frame_gate()` - yields 1 tick per chunk when gate active, reducing cache pollution during HTTP download
- **Before `cJSON_Parse()`** (line ~364): Full wait loop (cJSON is monolithic and cache-heavy):
  ```c
  while (display_renderer_is_frame_rendering && display_renderer_is_frame_rendering()) {
      vTaskDelay(1);
  }
  ```

### 3. Giphy refresh: yield before merge and eviction

**`components/giphy/giphy_refresh.c`**

Same weak extern + yield helper.

Insert yield points:
- **Before `giphy_merge_entries()`** (line ~446): Full wait for frame gate to clear
- **Before `giphy_evict_orphans()`** (line ~494): Full wait for frame gate to clear

### 4. Download manager: yield before each download cycle

**`components/channel_manager/download_manager.c`**

Same weak extern + yield helper (already has `animation_player_is_sd_paused` pattern to follow).

- **Before starting each file download** (in the download loop): `yield_for_frame_gate()`

### 5. Giphy download: yield between chunks

**`components/giphy/giphy_download.c`**

Same weak extern + yield helper.

- **After each 32KB chunk write to SD** (in the download loop): `yield_for_frame_gate()`

## Files to Modify

| File | Change |
|------|--------|
| `main/display_renderer.c` | Add `g_frame_rendering_active` flag + gate functions + wrap callback |
| `main/include/display_renderer.h` | Declare gate functions |
| `components/giphy/giphy_api.c` | Weak extern + yield in HTTP loop + wait before cJSON_Parse |
| `components/giphy/giphy_refresh.c` | Weak extern + yield before merge + eviction |
| `components/giphy/giphy_download.c` | Weak extern + yield between download chunks |
| `components/channel_manager/download_manager.c` | Weak extern + yield before downloads |

## Key design decisions

- **Volatile bool, not mutex/event group**: Zero overhead for the render task (no system calls). Network tasks only pay `vTaskDelay(1)` cost when gate is active. Cache coherency of the flag itself is guaranteed since it's a single word.
- **Weak symbol pattern**: Components don't hard-depend on `main/`. If the gate functions aren't linked (e.g., unit tests), the NULL check prevents calls.
- **Gate wraps entire callback, not just decode**: Simpler code. The callback includes brief mutex ops and state management, but decode+upscale dominates (~95% of callback time).
- **Light yield (`vTaskDelay(1)`) in HTTP receive loop**: Don't want to block the entire download on every chunk - just briefly yield to let the render task's cache accesses proceed uncontested.
- **Full wait before cJSON_Parse**: cJSON is monolithic and heavily cache-polluting. Wait for current frame decode to complete before starting the parse.

## Verification

1. Build and flash
2. Play a WebP animation, trigger a Giphy channel refresh (or wait for automatic refresh)
3. Observe smooth animation playback (no visible stutter/jank)
4. Check logs: Giphy refresh should still complete successfully (all pages fetched, merge/eviction done)
5. Refresh may take slightly longer (seconds, not minutes) due to yields - this is expected
