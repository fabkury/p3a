# p3a — Top 10 Shortcomings

An honest assessment of the most impactful weaknesses in the p3a codebase as of v0.8.5-dev. Each shortcoming includes evidence from specific files and a suggested remediation path.

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
