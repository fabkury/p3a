# Firmware internals

Part of the [2.0 architecture review](README.md). This page covers the
runtime skeleton: tasks, memory, the state machine, the event bus, the
service layer, component dependencies, configuration tiers, error handling
and testability. The content pipeline, storage formats and the HTTP/web UI
layer have their own pages.

Line numbers refer to `main` at `1692d43a`.

## 1. Task model: 42 tasks, two stack policies, three priority conventions

**What 1.x does.** Project code creates 42 distinct tasks (46 `xTaskCreate*`
sites minus four debug-only ones). Twenty use the PSRAM-static pattern
(`heap_caps_malloc(MALLOC_CAP_SPIRAM)` for the stack, `xTaskCreateStatic*`,
internal-RAM fallback); twenty-two use plain dynamic internal stacks. The
PSRAM pattern is a 15-line block copy-pasted at every site; the comment
"pinned to core 0: jitter work stream fix 7b" appears 33 times.

Priorities come from three places at once. `main/Kconfig.projbuild:95-144`
defines RENDER=8, TOUCH=6, APP=4, NETWORK=3. Nine production sites use the
literal `5`, which outranks APP and NETWORK: `pin_io`
(`components/p3a_core/p3a_pin_dispatcher.c:257`), `giphy_click` and
`reaction_mqtt` (`p3a_reaction_dispatcher.c:111,205`), `webui_install` and
`webui_repair` (`components/ota_manager/ota_manager_webui.c:718,774`),
`sd_format`, `touch_init`, `sdf_touch_init`. `download_mgr` uses literal `3`
(`components/channel_manager/download_manager.c:1045`), `show_url` uses
`idle+2`, `touch_rescue` uses `configMAX-2`. A one-shot pin write therefore
preempts the scheduler, the event bus and every network task.

Twelve tasks exist to run one blocking call and exit: `pin_io`, `giphy_click`,
`reaction_mqtt`, `usb_msc_reboot`, `usb_msc_recv`, `transport_ui`,
`transport_rbt`, `cert_renew`, `ota_check`, `webui_install`, `touch_init`,
`sdf_touch_init`. Each allocates an internal-RAM stack at the moment it is
spawned, which is usually when the network is busy, which is exactly the
pressure point of the open SDIO RX issue in `docs/known-issues.md`.

There is no shared worker. `api_worker` (`components/http_api/http_api.c:85-135`)
is a 10-deep queue of five enum commands that re-emit event-bus events.
`ps_switch` is single-purpose. The event-bus dispatch task runs handlers
synchronously under a non-recursive mutex (`components/event_bus/event_bus.c:76-90`),
so a handler that emits and waits deadlocks. None of the three accepts an
arbitrary `{fn, arg}`; every new blocking job becomes a new task.

**Why it got here.** The jitter work stream moved networking to core 0 and
PSRAM stacks one task at a time, each as a measured fix. The one-shot tasks
were the quickest way to get a blocking call off the touch or HTTP task. Both
were right locally.

**Fresh design.** One `p3a_task_create(name, fn, arg, stack, class)` with
classes RENDER, TOUCH, APP, NETWORK, BACKGROUND that encodes priority, core
and PSRAM-by-default once. One or two long-lived job runners (I/O and UI)
taking `{fn, arg}` items replace the twelve one-shot tasks. Expected task
count: around 25, all with PSRAM stacks except the display pipeline and
touch.

**Migration cost.** Low to medium. The helper is a day; converting sites is
mechanical. The job runner needs care for the two tasks that today block for
minutes (`cert_renew`, `webui_install`), which simply go away or get their own
class.

**Verdict.** Would not build it this way. Fix in 2.0.

## 2. Memory: four allocation idioms and no pressure signal

**What 1.x does.** Four idioms coexist: `psram_malloc` and friends (about 80
sites, defined in `components/channel_manager/include/psram_alloc.h:32-61`),
raw `heap_caps_malloc(MALLOC_CAP_SPIRAM)` (43 sites), plain `malloc` (107
sites, including every museum adapter and `play_scheduler_playsets.c`), and
explicit `MALLOC_CAP_INTERNAL` for hot tables (4 sites). `cJSON` is hooked to
PSRAM in `app_main`; uthash is redirected to `psram_malloc` in
`channel_cache.h:32`. The allocation helper lives in a domain component, so
seven components depend on `channel_manager` partly to get a header.

`docs/known-issues.md` is accurate: the 272 KB DMA-capable pool is the
failure point and "nothing in the firmware sheds load on memory pressure".
`heap_caps_get_free_size` appears only in diagnostics (`main/p3a_main.c:147-161`,
`components/mem_stats/mem_stats.c:38-41`, `http_api.c:857-861`). The only
throttles are structural: `CONFIG_HTTP_FETCH_MAX_CONCURRENT_TLS=2` and
`REFRESH_MAX_CONCURRENT=2`. The watermark in `download_manager.c:930` is SD
free space, not RAM.

**Fresh design.** A leaf `p3a_util` component (no IDF app dependencies)
holding `psram_alloc.h`, `p3a_limits.h` and a `p3a_mem_pressure` watermark on
the DMA pool, published once and consumed by the download manager, the
refresh task and `http_fetch`. Plain `malloc` disappears from domain code;
PSRAM is the default and internal RAM is opt-in with a reason in the call.

**Migration cost.** Low for the move and the default; medium for the pressure
signal, which needs a soak test under a full refresh cycle before it is
trusted (known-issues already says so).

**Verdict.** Would not build it this way. Fix in 2.0. The pressure signal is
the one piece of this review that also addresses an open production crash.

## 3. The state machine is one of seven copies of "what is on screen"

**What 1.x does.** `components/p3a_core/p3a_state.h:64-71` declares six
states; `can_enter_state()` (`p3a_state.c:46-73`) guards three transitions.
`p3a_state_enter_error` skips the guard (`p3a_state.c:351-364`). Callbacks
fire after the mutex is released. Sub-states are unguarded setters
(`p3a_state.c:373-455`); `P3A_PLAYBACK_CHANNEL_MESSAGE` is derived from
whether a message string is set. `p3a_app_status_t` is a third orthogonal axis
driven only by `http_api` and `app_wifi`.

"What is on screen" is also held in: `s_render.channel_msg_*` with its own TTL
and priority logic (`p3a_render.c:339-421`), `ugfx_ui.c` `s_ui_mode` with 14
modes (`main/ugfx_ui.c:50-82`), `display_renderer` `g_display_mode_*`
(`main/display_renderer.c:28-29`), `playback_controller` (`main/include/playback_controller.h:27-31`),
`animation_player` (`s_anim_paused`, `s_swap_requested`, `s_front_buffer.ready`),
`playback_service.c:19` `s_paused`, and `p3a_current_post.c:17-21`.
`p3a_state_channel.c:52-58` writes the same message to both `p3a_render` and
`ugfx_ui`; `makapix_channel_switch.c` calls `p3a_render_set_channel_message`
nine times directly. The state machine is a parallel record, not the owner.

Forty-nine external call sites mutate state from 12 files (`http_api` 10,
`ota_manager` 9, `main` 8, `slave_ota` 4, `makapix` 4).

**Why it got here.** `ugfx_ui` and `display_renderer` predate `p3a_core`;
the state machine was added on top and never finished taking ownership.
PICO-8 added `playback_controller` as a second arbiter.

**Fresh design.** One screen model. Either `p3a_state` is the single source
and `p3a_render`, `ugfx_ui`, `display_renderer` are pure readers of it, or
`p3a_state` is deleted and `ugfx_ui.s_ui_mode` plus a small playback struct
become the model. The display pipeline exposes one function, "render the
current model into this buffer", and nothing else writes mode flags. With
PICO-8 gone, `playback_controller.c` has no second source to arbitrate and is
deleted.

**Migration cost.** Medium. The overlays and `ugfx_ui` screens are fine as
drawing code; the work is removing their private mode flags and routing every
"show X" call through one setter. Touch routing (`p3a_touch_router.c`, 27
weak externs into `main`) becomes a table keyed on the model.

**Verdict.** Would not build it this way. Fix in 2.0, together with the
weak-extern cleanup below, because they are the same problem.

## 4. The event bus is decorative; weak externs are the real bus

**What 1.x does.** 18 event types (`event_bus.h:26-51`). Fourteen of the 21
subscriptions are in `main/p3a_main.c:518-533`, and `handle_system_event`
(`p3a_main.c:224-262`) forwards each to a `p3a_state_on_*` call the emitter
could have made. Only `SWAP_NEXT` fan-in (8 emitters) and `CACHE_FLUSH`
debounce are load-bearing. For the same purposes components also call each
other directly: `wifi_manager` calls `makapix_channel_signal_wifi_connected`
and `makapix_connect_if_registered` while also emitting `WIFI_CONNECTED`.

Meanwhile 97 `__attribute__((weak))` externs act as the inter-component call
mechanism, 66 of them inside `p3a_core` (`p3a_touch_router.c` alone has 27,
naming `main`-side symbols such as `app_lcd_*`, `ugfx_ui_*`, `sd_format_*`,
`app_usb_report_touch`). A weak extern hides a dependency edge from CMake and
from the reader, and a missing symbol becomes a silent no-op rather than a
link error.

**Fresh design.** Decide what the bus is for. Keep it for genuine fan-out
(swap requests, connectivity changes) and have components subscribe directly
so `p3a_main` stops being a relay. Release the dispatch mutex before calling
handlers. Replace the weak externs with either explicit `REQUIRES` or one
registered hook table (`p3a_core` declares `p3a_ui_hooks_t`; `main` fills it
once at boot). Zero weak symbols in 2.0 is a reasonable hard rule.

**Migration cost.** Low per site, 97 sites. Most are mechanical.

**Verdict.** Would not build it this way. Fix in 2.0.

## 5. The service layer adds a layer without a boundary

Three of the four services in `main/` forward calls one-to-one:
`connectivity_service.c` (31 lines), `content_service.c` (40, whose comment
explains it cannot initialize `pin_lists` because it runs before SD mount),
`render_engine.c` (20). `playback_service.c` (138) is a real orchestrator
(pause saves and restores brightness across `animation_player`,
`view_tracker`, `app_lcd`, `play_scheduler`), but it lives in `main/`, so
`http_api` reaches it through `PRIV_REQUIRES main`
(`components/http_api/CMakeLists.txt:24`) and `p3a_touch_router` through a
weak extern.

**Fresh design.** Delete the three forwarders. Promote `playback_service`
into a `playback` component so no component depends on `main`.

**Migration cost.** Trivial.

## 6. The dependency graph is cyclic and `main` is a component in disguise

From the `REQUIRES` lines:

- `main` <-> `http_api` (`http_api PRIV_REQUIRES main`)
- `channel_manager` <-> `play_scheduler`
- `channel_manager` <-> `makapix`; `channel_manager` <-> `giphy`, `klipy`, `art_institution`
- `play_scheduler` <-> `giphy`, `klipy`, `art_institution`, `makapix`, `pin_lists`
- `wifi_manager` <-> `makapix` (both PRIV); `wifi_manager` -> `http_api` -> `main` -> `wifi_manager`
- `p3a_core` -> `p3a_board_ep44b` -> `slave_ota` -> `p3a_core`

CMake tolerates the cycles; the linker does not, which is why `play_scheduler`,
`giphy`, `klipy`, `art_institution` and `makapix` carry `WHOLE_ARCHIVE`
(`components/play_scheduler/CMakeLists.txt:18-24` explains the symptom
honestly). `http_api`, `makapix`, `pico8` and `art_institution` include
headers from `main/include` (`animation_player.h`, `app_lcd.h`,
`playback_service.h`, `ugfx_ui.h`, `display_renderer.h`, `version.h`).

`p3a_core` is required by 16 components and is a grab bag: state machine,
render dispatcher, touch router, pin and reaction dispatchers that spawn
tasks, SD path, health and repair, atomic writes, boot logo with a 52 KB
bitmap, fresh-boot NVS logic. It requires `lwip`, `esp_netif`, `sdmmc` and
`p3a_board_ep44b`, so everything that touches `sd_path` also links the network
stack's headers.

`main/` is 16,910 lines, of which about 1,300 are entry-point glue. The rest
is the display pipeline (about 2,800 lines), the animation player (2,568),
`ugfx_ui.c` (1,764, 28 public functions, called from four components), the
USB composite device, the on-device SD formatter and 4,203 lines of hex
image arrays (337 KB of C source; `EMBED_FILES` is used nowhere).

**Fresh design.** A layered graph with one direction:

```
p3a_util (alloc, limits, mem pressure)      no IDF app deps
  ^
board (display, touch, SD, button, brightness)
  ^
storage (sd_path, fs_atomic, sd_health, sd_repair, record store)
  ^
sources (makapix, giphy, klipy, museum, local, pinned)   implement one interface, know nothing above
  ^
library (index + blob store + download + refresh)        owns records, calls sources through the table
  ^
player (scheduler, loader, decoders, display pipeline)
  ^
ui (screen model, ugfx screens, touch router, overlays)
  ^
net (wifi, http_api, mqtt commands, ota)                 only layer that may call everything below
  ^
main (boot sequence only)
```

Sources never include the scheduler. `main` owns nothing but `app_main`.
Every arrow is a `REQUIRES`; `WHOLE_ARCHIVE` and weak externs are gone.

**Migration cost.** High if done as a refactor of 1.x, because the cycles are
load-bearing (see the content-pipeline page). Low if 2.0 is assembled in this
order from the start and 1.x modules are moved in one at a time, each move
stripping the upward includes.

**Verdict.** Would not build it this way. This is the single most important
structural change in the review and the one that makes every other page's
fixes cheap.

## 7. Three configuration tiers with no stated rule

94 Kconfig options (33 in `main/Kconfig.projbuild`, 61 across 15 component
Kconfigs). Eight have zero `CONFIG_` references in any source or CMake file:
`P3A_AUTO_SWAP_INTERVAL_SECONDS` (still listed in AGENTS.md as a main option),
`P3A_SD_ANIMATIONS_DIR`, `P3A_USE_PIE_SIMD`, `P3A_DISPLAY_WAIT_AFTER_DRAW`,
`CHANNEL_MANAGER_PAGE_SIZE`, `P3A_PICO8_MODE_TIMEOUT_MS`,
`WIFI_MANAGER_SNTP_SERVER`, `WIFI_MANAGER_ENABLED`. Fifteen `ESP_WIFI_*`
entries are inherited from the IDF station example. Runtime settings live in
NVS through `config_store` (see the storage page for its shape). Nothing
states which knob belongs in which tier.

**Fresh design.** Kconfig for build-shape only (features in or out, task
sizes, trace). A single settings schema for everything a user can change.
Compile-time constants in one `p3a_limits.h`. Write the rule down in AGENTS.md.

**Migration cost.** Trivial. Delete the dead options first.

## 8. Errors: no project error base, four user-facing surfaces

Zero hits for a project error base. Functions return bare `esp_err_t` and
callers log `esp_err_to_name`. The loader classifies failures by comparing
IDF error codes and matching path substrings (`/vault/`, `/giphy/`,
`/museum/`; `/klipy/` and pinned copies are missing from the list, so a
corrupt Klipy file is never deleted). `docs/deferred/aspect-ratio-filter.md`
already hit this wall: a policy rejection needs its own sentinel and the
project has none.

User-facing surfaces: REST error codes (175 call sites, 84 distinct string
codes), the on-screen channel message (17 direct `ugfx_ui_show_channel_message`
sites plus the `p3a_render` path), `ugfx_ui_show_fatal_error` (5),
`p3a_app_status_t` via `/status`. Log volume is 2,460 statements for 60k
lines with WARN plus ERROR outnumbering INFO (I 646, D 372, W 653, E 791).

**Fresh design.** `ESP_ERR_P3A_BASE` with a short enum: `NOT_PLAYABLE`
(policy), `CORRUPT`, `MISSING`, `RATE_LIMITED`, `AUTH`, `NO_KEY`, `SD_FAILED`.
Every surface maps from that enum. The loader stops reading path strings.

**Migration cost.** Low. It is one header plus a sweep of the loader and the
refresh ladder.

## 9. Testability: none, and the structure prevents it

No `test/` or `test_apps/`, no Unity or CMock, no build-on-push workflow
(`.github/workflows/` holds only `publish-firmware-to-pages.yml`). The only
host-compiled project code is `host/intro-anim-lab`. Pure logic (scheduler
pick, LAi, playset JSON, state transitions) sits in files that include
`p3a_state.h` and call `p3a_state_get()` (13 such calls in `play_scheduler`),
and `psram_alloc.h` is inline `heap_caps_*`, so nothing compiles on a host
without the IDF.

**Fresh design.** The layering in section 6 is what makes this possible:
`library`, the scheduler's pick and SWRR, the record store codec and the
settings schema have no IDF includes and compile on the host with a 100-line
FreeRTOS stub. A GitHub Actions job builds the firmware and runs the host
tests on every push. The new 2.0 features (repetition suppression, aspect
filter) are exactly the kind of logic that should have a host test before it
runs on a device.

**Migration cost.** Medium, paid once. It is the only item in this review
where doing it later costs more than doing it first.

## 10. What is good and should be kept

- The display pipeline (producer, consumer, vsync-phase-locked playhead,
  generation-based flush) is well designed and well documented in
  `docs/infrastructure/display-pipeline.md`. Keep it as is, as a component.
- The jitter fixes (`sd_idle_wait`, aligned DMA buffers, POSIX reads, band
  stealing) are measured and should carry over untouched.
- `http_fetch` as the single HTTPS path with a TLS gate is the right shape;
  the rest of the networking should look more like it.
- `fs_atomic`, `sd_health` and `sd_repair` are the right primitives (see the
  storage page for how to build on them).
- `frame_trace` and `mem_stats` compile to nothing when off and are worth
  keeping as the diagnostic floor.
- The decoders behind `animation_decoder` are a clean interface over five
  formats and need no change.
