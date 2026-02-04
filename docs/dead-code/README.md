# Dead Code Analysis Report

This document provides a systematic analysis of potential dead code in the p3a repository.

**Analysis Date:** 2025-02-03  
**Scope:** All C/C++ source files, headers, configurations, scripts, and assets

---

## Executive Summary

The p3a codebase is generally well-maintained with minimal dead code. However, there are some areas that could be cleaned up:

| Category | Count | Impact |
|----------|-------|--------|
| Unused Components | 3 | Medium - Can be safely removed |
| Deprecated Functions | ~15 | Low - Marked for future removal |
| Unused Assets | 3 | Negligible - Simple deletions |
| Unused Config Options | 1-2 | Low - Kconfig cleanup |
| Debug/Development Code | 5-8 | Negligible - Explicitly marked |

---

## Table of Contents

1. [Unused Components](#1-unused-components)
2. [Deprecated Functions and APIs](#2-deprecated-functions-and-apis)
3. [Unused Static Functions](#3-unused-static-functions)
4. [Unused Configuration Options](#4-unused-configuration-options)
5. [Unused Assets and Files](#5-unused-assets-and-files)
6. [Example/Test Code in Production](#6-exampletest-code-in-production)
7. [Commented-Out Code Blocks](#7-commented-out-code-blocks)
8. [Recommendations](#8-recommendations)

---

## 1. Unused Components

### 1.1 `components/sync_playlist/`

**Status:** Partially used (only by `live_mode`)  
**Impact:** Medium  
**Location:** `components/sync_playlist/`

The sync_playlist component is designed for Live Mode synchronized playback. Currently:
- Only referenced by `components/live_mode/swap_future.c`
- Live Mode itself is marked as "DEFERRED" in the codebase
- Contains a working implementation but no active consumers in main application

**Files:**
- `sync_playlist.c` - Full implementation (~200 lines)
- `sync_playlist.h` - Public API
- `example.c` - Example code that should not be in production

**Recommendation:** Keep if Live Mode is planned for re-implementation. Mark as experimental/deferred.

---

### 1.2 `components/content_source/`

**Status:** Completely unused  
**Impact:** Low  
**Location:** `components/content_source/`

This component defines a `content_source_t` abstraction layer over channels. It's:
- Never called from `main/` or other components
- Only referenced in its own files and a design document (`docs/first-principles/v2/high-level/03-content-pipeline-refactoring.md`)
- Appears to be a planned abstraction that was never integrated

**Files:**
- `content_source.c` - Implementation (~47 lines)
- `include/content_source.h` - Public API

**Recommendation:** Remove unless it's part of an active refactoring plan.

---

### 1.3 `components/debug_http_log/`

**Status:** Conditionally compiled (CONFIG_P3A_PERF_DEBUG)  
**Impact:** Negligible  
**Location:** `components/debug_http_log/`

This performance debugging component is:
- Guarded by `#if CONFIG_P3A_PERF_DEBUG` which is not defined by default
- Called from `animation_player_render.c` and `webp_animation_decoder.c` when enabled
- Not actually "dead" but disabled in production

**Recommendation:** Keep - This is proper conditional compilation for debug features.

---

## 2. Deprecated Functions and APIs

### 2.1 Animation Player Deprecated API

**Location:** `main/include/animation_player.h` (lines 82-94)

```c
// DEPRECATED API (to be removed after refactor)

/**
 * @deprecated Use play_scheduler_next/play_scheduler_prev instead
 */
void animation_player_cycle_animation(bool forward);

/**
 * @deprecated Use play_scheduler_play_named_channel instead
 */
esp_err_t animation_player_request_swap_current(void);
```

**Note:** `animation_player_request_swap_current()` is declared twice (lines 93 and 113).

---

### 2.2 Channel Navigation Functions (Deprecated)

**Locations:**
- `components/channel_manager/sdcard_channel_impl.c` (lines 13-16)
- `components/channel_manager/makapix_channel_impl.c`

```c
// NOTE: play_navigator was removed as part of Play Scheduler migration.
// Navigation is now handled by Play Scheduler directly.
// The legacy navigation functions below return ESP_ERR_NOT_SUPPORTED.
```

**Affected functions:**
- `sdcard_impl_next_item()`
- `sdcard_impl_prev_item()`
- `sdcard_impl_current_item()`
- `makapix_impl_next_item()`
- `makapix_impl_prev_item()`
- `makapix_impl_current_item()`

These functions exist but return `ESP_ERR_NOT_SUPPORTED`.

---

### 2.3 p3a_state Deprecated Functions

**Location:** `components/p3a_core/include/p3a_state.h` (lines 506-517)

```c
/**
 * @deprecated Use p3a_state_set_active_playset() instead
 */
esp_err_t p3a_state_persist_channel(void);

/**
 * @deprecated Channel persistence is being replaced by playset persistence
 */
esp_err_t p3a_state_load_channel(p3a_channel_info_t *out_info);
```

---

### 2.4 Other Deprecated Items

| Location | Item | Notes |
|----------|------|-------|
| `main/animation_player_priv.h` | `ANIMATIONS_PREFERRED_DIR` | Deprecated in favor of `sd_path_get_animations()` |
| `main/include/app_lcd.h:2` | `app_lcd_draw()` | "Draw to display (legacy, not used)" |
| `main/animation_player.h:165-167` | `animation_player_submit_pico8_frame()` | Use `pico8_render_submit_frame()` instead |
| `components/http_api/http_api_rest.c` | Live mode endpoints | "Live mode deprecated" |
| `components/play_scheduler/play_scheduler_compat.c` | Entire file | "Compatibility shims for legacy symbols" |

---

## 3. Unused Static Functions

These functions are marked with `__attribute__((unused))` and are intentionally kept for debugging or future use:

| File | Function | Purpose |
|------|----------|---------|
| `components/http_api/http_api.c` | `log_all_netifs()` | Debug helper |
| `components/http_api/http_api.c` | `verify_server_listening()` | Debug helper |
| `components/http_api/http_api.c` | `format_sockaddr()` | Debug helper |
| `components/makapix/makapix.c` | `create_single_artwork_channel()` | Unused feature |
| `components/channel_manager/makapix_channel_impl.c` | `get_entry_flags()` | Unused helper |
| `components/play_scheduler/play_scheduler_pick.c` | `has_404_marker()` | Unused helper |
| `main/app_touch.c` | `get_next_rotation_cw()` | Unused helper |
| `main/app_touch.c` | `get_next_rotation_ccw()` | Unused helper |

**Recommendation:** These are intentional and properly marked. No action needed unless functions are truly unnecessary.

---

## 4. Unused Configuration Options

### 4.1 `CONFIG_PLAY_SCHEDULER_RANDOM_WINDOW`

**Location:** `components/play_scheduler/Kconfig`  
**Status:** Defined but never used in code

```
config PLAY_SCHEDULER_RANDOM_WINDOW
    int "Random pick window size"
    default 64
    range 8 256
    help
        Size of random window for RandomPick mode.
```

This Kconfig option is defined but the code doesn't reference `CONFIG_PLAY_SCHEDULER_RANDOM_WINDOW` anywhere.

**Recommendation:** Either implement usage in RandomPick mode or remove from Kconfig.

---

### 4.2 `CONFIG_CHANNEL_DEFAULT_ANIMATIONS_DIR`

**Location:** `components/channel_manager/Kconfig`  
**Status:** Deprecated

Only used in a legacy macro definition:
```c
#ifdef CONFIG_CHANNEL_DEFAULT_ANIMATIONS_DIR
#define ANIMATIONS_DEFAULT_DIR CONFIG_CHANNEL_DEFAULT_ANIMATIONS_DIR
#endif
```

Modern code uses `sd_path_get_animations()` instead.

---

## 5. Unused Assets and Files

### 5.1 Images

| File | Status |
|------|--------|
| `/images/PICO-8_logo.png` | ❌ No references in codebase |
| `/images/PICO-8_logo_35p.gif` | ❌ No references in codebase |
| `/webui/static/pico8_logo.png` | ❌ No references in HTML/CSS/JS |

**Recommendation:** Remove these unused image files.

---

### 5.2 Certificate Documentation

| File | Status |
|------|--------|
| `/certs/information_for_physical_player.md` | ⚠️ No references, likely documentation |

---

### 5.3 Old Release Directories

**Location:** `/release/`

Contains 13 historical release versions (v0.6.0-dev through v0.7.8-dev). Older versions like v0.6.x may no longer be needed.

**Recommendation:** Consider archiving releases older than 2-3 versions back.

---

## 6. Example/Test Code in Production

### 6.1 `components/sync_playlist/example.c`

**Location:** `components/sync_playlist/example.c`

This file contains example usage code with a sample `app_main()` function. It should not be compiled into production builds.

```c
// Example: Initialize the Synchronized Playlist Engine.
void app_main(void)
{
    // Initialize animations (for example purposes, durations are illustrative)
    animation_t animations[] = { ... };
    ...
}
```

**Recommendation:** Either:
1. Exclude from CMakeLists.txt SRCS
2. Move to a `/examples/` directory
3. Remove entirely if example is obsolete

---

## 7. Commented-Out Code Blocks

### 7.1 Third-Party Libraries (Low Priority)

These are in third-party code and should not be modified:

| File | Description |
|------|-------------|
| `components/ugfx/src/gmisc/gmisc.h` | `#if 0` block |
| `components/ugfx/src/gdisp/gdisp_image_jpg.c` | `#if 0` block |
| `components/ugfx/src/gwin/gwin_keyboard.c` | Commented keyboard event filtering |
| `components/animated_gif_decoder/gif.inl` | Commented optimization attempts |

**Recommendation:** Leave as-is - these are upstream third-party files.

---

### 7.2 TODO Comments for Deferred Features

| File | Comment |
|------|---------|
| `components/channel_manager/playlist_manager.c` | "TODO: Implement background update queue" |
| `components/p3a_board_ep44b/include/p3a_board.h` | "TODO: Remove these after full migration" |
| `components/live_mode/swap_future.c` | Full block explaining deferred Live Mode |

These are planned features, not dead code.

---

## 8. Recommendations

### High Priority (Safe to Remove Now)

1. **Remove unused image assets:**
   - `/images/PICO-8_logo.png`
   - `/images/PICO-8_logo_35p.gif`
   - `/webui/static/pico8_logo.png`

2. **Remove or relocate `sync_playlist/example.c`**

3. **Remove `components/content_source/`** if not part of active development plans

### Medium Priority (Cleanup When Convenient)

4. **Implement or remove `CONFIG_PLAY_SCHEDULER_RANDOM_WINDOW`** Kconfig option

5. **Clean up duplicate declaration** of `animation_player_request_swap_current()` in `animation_player.h`

6. **Consider removing deprecated navigation functions** from channel implementations (or add explicit `__attribute__((deprecated))` markers)

### Low Priority (Document for Future)

7. **Archive old release versions** (v0.6.x series)

8. **Add deprecation timeline** to deprecated functions (e.g., "Will be removed in v1.0")

---

## Notes

- Code marked with `__attribute__((unused))` is intentionally kept and should not be removed without understanding why it was preserved
- Conditional compilation (`#if CONFIG_*`) is NOT dead code - it's proper feature gating
- Third-party libraries (ugfx, gif decoder) should not be modified even if they contain dead code
- The Live Mode features are deferred, not abandoned - sync_playlist should be kept if Live Mode is planned
