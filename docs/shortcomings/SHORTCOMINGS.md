# p3a — Top 10 Shortcomings

An honest assessment of the most impactful weaknesses in the p3a codebase as of v0.8.5-dev. Each shortcoming includes evidence from specific files and a suggested remediation path.

---

## 1. No Unit Tests or Integration Tests

The project has **zero** automated tests. There are no `test/` directories, no `*_test.c` files, no test runner configuration, and no testing framework integrated into the build system. The only file with "test" in its name (`gmisc_hittest.c`) is a hit-testing algorithm inside the vendored uGFX library, not a test.

**Impact:** Every change is validated only by manual flashing and observation. Regressions go undetected until a user encounters them. Components like `play_scheduler`, `config_store`, and `channel_manager` contain complex logic (SWRR balancing, NVS atomic saves, playlist resolution) that is well-suited to unit testing but currently relies entirely on ad-hoc verification.

**Remediation:** Adopt the [ESP-IDF unit test framework](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/unit-tests.html) or host-based testing with CMock/Unity. Start with pure-logic components (`play_scheduler`, `config_store`, `playset_json`) that can be tested without hardware.

---

## 2. No CI/CD Pipeline

There is no `.github/` directory, no GitHub Actions workflow, and no other CI configuration. Builds, linting, and release packaging are performed entirely on the developer's local machine.

**Impact:** Build breakage can be committed silently. There is no automated gate for pull requests — no compilation check, no static analysis, no binary size monitoring. Release binaries are produced manually, leaving room for inconsistent build environments or missed steps.

**Remediation:** Add a GitHub Actions workflow that, at minimum, compiles the project with `idf.py build` in the official `espressif/idf` Docker container. Later stages can add static analysis, binary size tracking, and automated release artifact publishing.

---

## 3. Thread Safety Gaps

Several shared data structures are accessed from multiple FreeRTOS tasks without consistent synchronization:

| Location | Issue |
|----------|-------|
| `p3a_render.c` (lines 24–42) | `s_render` struct written by `p3a_render_set_*` functions and read by the render loop without any mutex. |
| `p3a_state.c` (line 347) | `p3a_state_get_active_playset()` returns a pointer to an internal buffer after releasing the mutex; a concurrent `p3a_state_set_active_playset()` can cause torn reads. |
| `config_store.c` | Static caches (`s_bg_color`, `s_show_fps`, etc.) are read and written from multiple tasks without synchronization. |
| `playlist_manager.c` (line 21) | `s_current_playlist` and `s_current_playlist_id` are accessed without a mutex. |
| `sd_path.c` (line 15) | `s_root_path` and `s_initialized` are not protected; `sd_path_get_root()` can race with `sd_path_init()`. |
| `animation_player.c` (line 455) | `s_sd_export_active` is set without holding `s_buffer_mutex` in several paths, creating a TOCTOU window. |
| `display_renderer.c` (line 271) | `display_renderer_set_frame_callback` updates callback/context even when the mutex take fails. |
| `playback_controller.c` (line 76) | On mutex timeout, `s_controller.current_source` is read without the mutex held. |

**Impact:** Data races can cause corrupted state, visual glitches, or crashes that are extremely difficult to reproduce and diagnose.

**Remediation:** Audit every global/static variable accessed from more than one task. Protect shared state with mutexes (or use atomic operations for simple flags). For `p3a_state_get_active_playset`, return a copy instead of a pointer to internal storage.

---

## 4. Mismatched Memory Allocators

The codebase mixes `heap_caps_malloc` (PSRAM-targeted) and `malloc`/`free` inconsistently:

| Location | Issue |
|----------|-------|
| `animation_player_loader.c` (lines 516–524 vs 414–415) | `native_frame_b1` / `native_frame_b2` are allocated with `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` but freed with plain `free()`. |
| `playlist_manager.c` (line 278 vs 367) | Playlist artworks allocated with `psram_calloc` but freed with `free()`. |
| `giphy_refresh.c` (line 77 vs 117) | Cache entries allocated with `psram_malloc` but freed with `free()`. |
| `channel_cache.c` | Mixes `malloc`/`free` and `psram_malloc` for related data structures. |

**Impact:** On the ESP32-P4, `heap_caps_malloc` with `MALLOC_CAP_SPIRAM` returns memory from the PSRAM heap. Passing that pointer to plain `free()` works *only* because ESP-IDF's default allocator unifies heaps — but this is an implementation detail, not a contract. If the heap strategy changes, or if tracing allocators are enabled, these mismatches will surface as corruption or crashes.

**Remediation:** Establish a project convention: either always use `heap_caps_malloc` / `heap_caps_free` for PSRAM, or rely on the unified allocator and use `malloc` / `free` everywhere. Wrapper macros (`p3a_psram_alloc`, `p3a_psram_free`) would make the intent explicit and the policy enforceable.

---

## 5. Pervasive Hardcoded Magic Numbers

Timeouts, buffer sizes, stack sizes, and limits are scattered as raw numeric literals throughout the code instead of being defined as named constants or Kconfig options:

| Value | Location | Purpose |
|-------|----------|---------|
| `8192` | `animation_player.c:346`, `play_scheduler_refresh.c` | Task stack sizes |
| `4096`, `2048`, `3072` | `display_renderer.c:254,167`, `p3a_main.c:409` | More task stack sizes |
| `100` (ms) | `animation_player.c:489`, `animation_player_render.c:23` | Timeout, default frame delay |
| `5000`, `15000`, `120000` | `animation_player_loader.c:216`, `giphy_api.c:165`, `download_manager.c:411` | HTTP / bus timeouts |
| `256`, `512` | `animation_player_loader.c:282,305` | Path buffer sizes |
| `5 * 1024 * 1024` | `http_api_upload.c:28` | Max upload file size |
| `18 * 3600` | `download_manager.c:368` | Channel eviction interval |
| `3600000` | `animation_player_loader.c:47` | Corrupt file cooldown |
| `60000`, `300000` | `p3a_state.c` | Internet check, MQTT backoff |

**Impact:** These values cannot be tuned without editing source code and rebuilding. Related constants are defined in different files with no single source of truth — for example, path buffer sizes of 256 and 512 appear independently in multiple files. If one is changed but another is not, truncation or overflow may occur.

**Remediation:** Centralize constants in a project-wide `p3a_constants.h` or, where runtime tuning is desirable, expose them through Kconfig. Group related constants (all task stack sizes, all timeout values, all buffer sizes) so they can be reviewed and adjusted together.

---

## 6. Missing Input Validation and Sanitization

Several HTTP and MQTT code paths accept external input without adequate validation:

| Location | Issue |
|----------|-------|
| `http_api.c` (line 336) | The `swap_to` endpoint accepts a `filename` from JSON and uses it to build a file path with `snprintf`. Unlike the `/upload` endpoint, there is **no path-traversal check** — a crafted filename containing `../` could access files outside the animations directory. |
| `http_api.c` (lines 298, 467) | JSON responses are built with `snprintf` embedding user-controlled strings (`final_filename`, `art_url`) without JSON escaping. A filename containing `"` or `\` will produce invalid JSON; a crafted filename could inject arbitrary JSON fields. |
| `playset_json.c` (line 139) | `cJSON_GetStringValue` may return NULL, but the result is passed to `strncpy`/`strlcpy` without a NULL check. |
| `giphy_api.c` (line 115) | `url_encode` assumes the output buffer is large enough; there is no explicit overflow check. |

**Impact:** The path-traversal gap in `swap_to` is a security vulnerability on the local network. The JSON injection issues can cause API clients to malfunction. NULL pointer dereferences in JSON parsing can crash the device.

**Remediation:** Apply the same path-traversal defense used in `http_api_upload.c` (basename extraction + rejection of `..`) to all endpoints that accept filenames. Use a proper JSON builder (or at minimum, escape special characters) when constructing JSON responses. Add NULL checks after every `cJSON_GetStringValue` call.

---

## 7. Incomplete Error Recovery

Many initialization and runtime failures are logged but not handled, leaving the system in a degraded state with no recovery path:

| Location | Behavior |
|----------|----------|
| `p3a_main.c` (lines 316–367) | Failures of `p3a_state_init`, `event_bus_init`, `content_service_init`, and `playback_service_init` are logged but execution continues. The system runs in an undefined partial state. |
| `display_renderer.c` (line 364) | If `ugfx_ui_set_rotation()` fails, the error is logged and `config_store_set_rotation` is still called. Config and actual display orientation diverge permanently. |
| `play_scheduler.c` (line 283) | Timer and refresh-task creation failures are logged with no retry or fallback. The scheduler silently stops refreshing. |
| `p3a_state.c` (line 636) | `p3a_state_connectivity_deinit()` deletes the internet-check timer without stopping it first; `xTimerDelete` on an active timer can cause a use-after-free in the timer daemon. |
| `download_manager.c` (line 531) | On init failure after creating the mutex, the mutex is not deleted. |

**Impact:** A single subsystem failure (e.g., event bus out of memory) cascades silently into unpredictable behavior. The device may appear to work but miss events, skip artworks, or fail to respond to touch. The user has no way to know what went wrong or trigger a recovery.

**Remediation:** Define a clear policy for init failures: either halt boot and display a diagnostic screen, or implement retry logic for recoverable errors. For runtime failures, consider a watchdog or health-check task that can reset degraded subsystems.

---

## 8. No Static Analysis or Linting in the Workflow

There is no `.clang-tidy`, `.cppcheck`, or other static-analysis configuration in the repository. The root `CMakeLists.txt` globally suppresses `-Wno-ignored-qualifiers`, reducing the compiler's ability to catch type-safety issues.

**Impact:** Entire categories of bugs — uninitialized variables, dead code, implicit casts, format-string mismatches — can only be caught by manual code review. The global warning suppression means even the compiler's built-in checks are weakened.

**Remediation:** Add a `.clang-tidy` configuration targeting at least `bugprone-*`, `cert-*`, and `readability-*` checks. Remove the global `-Wno-ignored-qualifiers` and fix the warnings it was hiding. Integrate static analysis into the (future) CI pipeline.

---

## 9. Security Hardening Not Enabled

The `sdkconfig` shows that both **Secure Boot** and **Flash Encryption** are disabled:

```
# CONFIG_SECURE_BOOT is not set
# CONFIG_SECURE_FLASH_ENC_ENABLED is not set
```

Additionally:
- `CONFIG_BOOT_ROM_LOG_ALWAYS_ON=y` — boot ROM logs are always printed, potentially leaking internal details.
- `CONFIG_ESP_DEBUG_OCDAWARE=y` — JTAG/debug awareness is on.
- Wi-Fi credentials in sdkconfig (`CONFIG_ESP_WIFI_SSID="myssid"`, `CONFIG_ESP_WIFI_PASSWORD="mypassword"`) are documented as examples but could be mistaken for real credentials.

**Impact:** Without secure boot, a malicious firmware image can be flashed via UART or OTA. Without flash encryption, all firmware and NVS data (including stored Wi-Fi credentials and API keys) are readable by anyone with physical access to the board.

**Remediation:** Enable secure boot and flash encryption before any production deployment. For development builds, document clearly that these are intentionally disabled. Replace the example Wi-Fi credentials in sdkconfig with empty strings and move provisioning to the captive portal flow exclusively.

---

## 10. ~~Stale and Inconsistent Documentation~~ (Resolved)

The following documentation issues have been fixed:

- **Missing ROADMAP.md** — Removed broken links from `README.md` and `CLAUDE.md`.
- **Missing OTA_IMPLEMENTATION_PLAN.md** — Removed broken link from `README.md`.
- **Phantom `content_source` component** — Removed from `architecture.md`, `components.md`, and `directory-structure.md`.
- **Undocumented `storage_eviction`** — Added to `components.md` (section 17) and `directory-structure.md`.
- **Component count mismatch** — Updated from "24" to "25" in `components.md` and `directory-structure.md`.
- **Legacy `app_state` references** — Removed from `components.md`, `directory-structure.md`, and `README.md`.

**Remaining (not documentation issues):** `p3a_board.h:298` TODO *"Remove these after full migration"* and `playlist_manager.c:217` TODO *"Implement background update queue"* are code-level items tracked separately.

---

## Summary

| # | Shortcoming | Severity | Effort to Fix |
|---|-------------|----------|---------------|
| 1 | No automated tests | High | High |
| 2 | No CI/CD pipeline | High | Medium |
| 3 | Thread safety gaps | High | High |
| 4 | Mismatched memory allocators | High | Low |
| 5 | Pervasive magic numbers | Medium | Medium |
| 6 | Missing input validation | High | Medium |
| 7 | Incomplete error recovery | Medium | High |
| 8 | No static analysis | Medium | Low |
| 9 | Security hardening disabled | Medium | Medium |
| 10 | ~~Stale documentation~~ | ~~Low~~ | ~~Low~~ |
