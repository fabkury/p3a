# Dead Code Cleanup Checklist

A prioritized list of cleanup tasks for dead code removal.

**Last updated:** 2026-02-24

---

## Quick Wins (5-10 minutes each)

### 1. Remove Unused Image Assets -- DONE

Removed `images/PICO-8_logo.png`, `images/PICO-8_logo_35p.gif`, `webui/static/pico8_logo.png`.

---

### 2. Fix Duplicate Declaration -- DONE

Removed both declarations of `animation_player_request_swap_current()` from `main/include/animation_player.h` (function was dead -- zero callers). Also removed the implementation from `animation_player.c`.

---

## Medium Effort (30-60 minutes each)

### 3. Remove Deprecated `animation_player_cycle_animation()` -- DONE

Removed function declaration, implementation, `s_cycle_pending`/`s_cycle_forward` globals,
their extern declarations in `animation_player_priv.h`, and the entire deferred-cycle block
in `animation_player_loader.c` (which was already a dead path logging "deprecated").

---

### 4. Remove Unused `content_source` Component -- DONE

Deleted `components/content_source/` (3 files). No callers existed outside the component.

---

### 5. Clean Up Unused Kconfig Options -- DONE

- Removed `CONFIG_PLAY_SCHEDULER_RANDOM_WINDOW` from `components/play_scheduler/Kconfig` (defined but never referenced in code).
- Removed `CONFIG_CHANNEL_DEFAULT_ANIMATIONS_DIR` from `components/channel_manager/Kconfig` and the `ANIMATIONS_DEFAULT_DIR` macro from `sdcard_channel.h`. Migrated the two call sites in `sdcard_channel.c` and `sdcard_channel_impl.c` to use `sd_path_get_animations()`.

---

### 6. Remove Deprecated Navigation Functions -- DONE

Removed `next_item`/`prev_item`/`current_item` function pointers from `channel_ops_t` in `channel_interface.h`, the three convenience macros, and all implementations in `sdcard_channel_impl.c`, `makapix_channel_impl.c`, and `makapix_single_artwork.c`. All implementations were stubs returning `ESP_ERR_NOT_SUPPORTED`. Navigation is handled by Play Scheduler.

---

### 7. Remove Deprecated `animation_player_submit_pico8_frame()` -- DONE

Removed declaration from `animation_player.h` and wrapper implementation from `animation_player.c`. All callers already use `pico8_render_submit_frame()` directly.

---

### 8. Remove Deprecated `app_lcd_draw()` -- DONE

Removed declaration from `app_lcd.h` and empty stub from `app_lcd_p4.c`.

---

### 9. Remove Stale Comments -- DONE

Removed orphaned `ANIMATIONS_PREFERRED_DIR` deprecation comment from `animation_player_priv.h`.

---

## Low Priority (When Refactoring)

### 10. Archive Old Releases

**Directory:** `release/`

v0.6.x releases have already been removed. Consider archiving v0.7.x releases when convenient.

---

### 11. Remove Legacy Migration Code -- DONE

Removed `is_legacy_format()` and `load_legacy_format()` from `channel_cache.c`.
Simplified `channel_cache_load()` to only attempt the current format; invalid/legacy files
are deleted and an empty cache is created from scratch.

---

### 12. Remove Deprecated `p3a_state` Functions -- DONE

Removed `p3a_state_persist_channel()`, `p3a_state_load_channel()`, and
`p3a_state_get_default_channel()` from `p3a_state.c` / `p3a_state.h`.
Removed the NVS key constants `NVS_KEY_CHANNEL_TYPE` / `NVS_KEY_CHANNEL_IDENT` and the
helper functions `channel_type_to_string()` / `string_to_channel_type()`.
Replaced the legacy channel load in `p3a_state_init()` with a fresh SD card default.
Channel persistence is now handled entirely by the playset system.

---

## Do NOT Remove

The following items look like dead code but should be kept:

| Item | Reason |
|------|--------|
| `__attribute__((unused))` functions | Intentional debug/development helpers |
| `#if 0` blocks in ugfx | Third-party upstream code |
| Commented code in gif.inl | Third-party upstream code |
| pico8_stream_stubs.c | Provides no-op implementations for disabled feature |
| debug_http_log component | Proper feature gating with CONFIG_P3A_PERF_DEBUG |

---

## Validation Checklist

After any cleanup, verify:

- [ ] `idf.py build` succeeds
- [ ] No new warnings introduced
- [ ] Basic functionality still works (animation playback, touch, WiFi)
- [ ] OTA update mechanism unaffected
- [ ] HTTP API still responds
- [ ] PICO-8 streaming still works (if enabled)
