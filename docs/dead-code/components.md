# Component-by-Component Dead Code Analysis

Detailed analysis of each component in the p3a repository.

---

## Main Application (`main/`)

### Status: ✅ Clean

| File | Dead Code Found | Notes |
|------|-----------------|-------|
| `p3a_main.c` | ❌ None | Well-maintained entry point |
| `animation_player.c` | ⚠️ Minor | Contains deprecated API implementations |
| `animation_player_loader.c` | ❌ None | Active loader logic |
| `animation_player_render.c` | ❌ None | Active render pipeline |
| `app_lcd_p4.c` | ⚠️ Minor | `app_lcd_draw()` marked as legacy |
| `app_touch.c` | ⚠️ Minor | Unused rotation helpers (`get_next_rotation_cw/ccw`) |
| `app_usb.c` | ❌ None | Properly feature-gated |
| `connectivity_service.c` | ❌ None | Active |
| `content_service.c` | ❌ None | Active |
| `display_fps_overlay.c` | ❌ None | Active |
| `display_processing_notification.c` | ❌ None | Active |
| `display_renderer.c` | ❌ None | Core rendering, active |
| `display_upscaler.c` | ❌ None | Active |
| `playback_controller.c` | ❌ None | Active |
| `playback_service.c` | ❌ None | Active |
| `render_engine.c` | ❌ None | Active |
| `ugfx_ui.c` | ❌ None | Active UI layer |

### Headers (`main/include/`)

| File | Issues |
|------|--------|
| `animation_player.h` | Duplicate declaration of `animation_player_request_swap_current()` |
| `app_lcd.h` | `app_lcd_draw()` marked "legacy, not used" |

---

## Components Analysis

### ✅ `animated_gif_decoder`
**Status:** Active, third-party library wrapper  
**Dead Code:** Minor commented-out code in `gif.inl` (upstream, don't modify)

### ✅ `animation_decoder`
**Status:** Active  
**Dead Code:** None

### ✅ `channel_manager`
**Status:** Active with deprecated navigation functions  
**Dead Code:**
- `sdcard_channel_impl.c`: Deprecated `next_item`, `prev_item`, `current_item` functions (return ESP_ERR_NOT_SUPPORTED)
- `makapix_channel_impl.c`: Same pattern
- `makapix_channel_impl.c`: Unused `get_entry_flags()` function
- `channel_cache.c`: Contains legacy format migration code (`is_legacy_format()`, `load_legacy_format()`) - may be removable if migration is complete

### ✅ `config_store`
**Status:** Active  
**Dead Code:** None

### ⚠️ `content_cache`
**Status:** Active (used by animation_player)  
**Dead Code:** None - despite initial suspicion, this IS used

### ❌ `content_source`
**Status:** **UNUSED**  
**Dead Code:** Entire component is never called from main application  
**Recommendation:** Remove or document as planned feature

### ⚠️ `debug_http_log`
**Status:** Conditionally compiled (CONFIG_P3A_PERF_DEBUG)  
**Dead Code:** Not dead - proper feature flag usage  
**Note:** The calls exist in `animation_player_render.c` and `webp_animation_decoder.c`

### ✅ `event_bus`
**Status:** Active  
**Dead Code:** None

### ✅ `http_api`
**Status:** Active  
**Dead Code:**
- `http_api.c`: Three debug functions marked `__attribute__((unused))`:
  - `log_all_netifs()`
  - `verify_server_listening()`
  - `format_sockaddr()`
- `http_api_rest.c`: "Live mode deprecated" endpoints

### ✅ `libwebp_decoder`
**Status:** Active (wrapper for external libwebp)  
**Dead Code:** `webp_decoder_component.c` is just a 6-line comment wrapper - not actual dead code

### ⚠️ `live_mode`
**Status:** DEFERRED feature  
**Dead Code:** The entire Live Mode implementation is marked as deferred, but code is intentionally preserved for future implementation. Key file:
- `swap_future.c` lines 16-33 explain the deferral

### ✅ `loader_service`
**Status:** Active  
**Dead Code:** None

### ✅ `makapix`
**Status:** Active  
**Dead Code:**
- `makapix.c`: `create_single_artwork_channel()` marked `__attribute__((unused))`

### ✅ `ota_manager`
**Status:** Active  
**Dead Code:** None

### ✅ `p3a_board_ep44b`
**Status:** Active  
**Dead Code:** Has TODO comment "Remove these after full migration" but code is still used

### ✅ `p3a_core`
**Status:** Active  
**Dead Code:**
- `p3a_state.h`: Two deprecated functions (`p3a_state_persist_channel`, `p3a_state_load_channel`)

### ✅ `pico8`
**Status:** Active (feature-gated)  
**Dead Code:**
- `pico8_stream_stubs.c`: Not dead - provides no-op implementations when PICO-8 is disabled

### ⚠️ `play_scheduler`
**Status:** Active  
**Dead Code:**
- `play_scheduler_pick.c`: `has_404_marker()` marked `__attribute__((unused))`
- `play_scheduler_compat.c`: Contains "Compatibility shims for legacy symbols"
- Kconfig: `CONFIG_PLAY_SCHEDULER_RANDOM_WINDOW` defined but never used

### ✅ `playback_queue`
**Status:** Active (used by animation_player)  
**Dead Code:** None

### ✅ `sdio_bus`
**Status:** Active  
**Dead Code:** None

### ✅ `slave_ota`
**Status:** Active (called from p3a_main.c)  
**Dead Code:** None

### ⚠️ `sync_playlist`
**Status:** Partially used (only by live_mode which is deferred)  
**Dead Code:**
- `example.c`: Example code that should not be in production build

### ✅ `ugfx`
**Status:** Active (third-party UI library)  
**Dead Code:** Various `#if 0` blocks and commented code - upstream code, do not modify

### ✅ `wifi_manager`
**Status:** Active  
**Dead Code:** None

---

## Summary by Severity

### 🔴 Should Remove

| Item | Location | Reason |
|------|----------|--------|
| `content_source` component | `components/content_source/` | Completely unused |
| `example.c` | `components/sync_playlist/` | Example code in production |
| Unused image assets | `images/`, `webui/static/` | Never referenced |

### 🟡 Consider Removing/Cleaning

| Item | Location | Reason |
|------|----------|--------|
| `CONFIG_PLAY_SCHEDULER_RANDOM_WINDOW` | `components/play_scheduler/Kconfig` | Defined but unused |
| Deprecated navigation functions | `channel_manager/*_impl.c` | Return ESP_ERR_NOT_SUPPORTED |
| Duplicate declaration | `main/include/animation_player.h` | `animation_player_request_swap_current()` declared twice |

### 🟢 Keep (Intentional)

| Item | Location | Reason |
|------|----------|--------|
| `__attribute__((unused))` functions | Various | Debug/development helpers |
| Live Mode code | `live_mode/`, `sync_playlist/` | Deferred feature, planned |
| Legacy migration code | `channel_cache.c` | May still be needed for old installations |
| Third-party commented code | `ugfx/`, `animated_gif_decoder/` | Upstream code, don't modify |
