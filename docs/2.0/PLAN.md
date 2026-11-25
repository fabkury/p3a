# 2.0 plan

Part of the [2.0 work stream](README.md). This page says what gets built, in
what order, and how each piece is known to be finished. It has no dates. The
order follows dependencies; status lives in [PROGRESS.md](PROGRESS.md).

The design behind every milestone is in
[the architecture review](../2.0-architecture-review/README.md). "Finding n"
below is row n of its verdict table. "Audit Hn/Mn" is an item in
`docs/known-issues.md`.

## Shape of the build

2.0 is assembled as a fresh layered tree (decision D3). The 1.x sources move
to `legacy/` at the first milestone and are never compiled again. Modules
leave `legacy/` one at a time, bottom layer first, and each arrives stripped
of upward dependencies and covered by tests. When `legacy/` is empty and the
parity checklist is complete, 2.0 is feature complete.

The layers, bottom to top (review, target architecture, section 1):

```
util      alloc, limits, error base, task helper, job runner, event bus, memory pressure
board     display, touch, SD mount, button, brightness, USB, co-processor flash
storage   sd_path, fs_atomic, sd_health, sd_repair, framed container, settings store
library   records, indices, blob store, the source interface, the HTTP fetcher, eviction
sources   makapix, giphy, klipy, museum, local, pinned; each fills p3a_source_ops_t (above library: D51)
player    scheduler, loader pipeline, decoders, upscalers, display pipeline
ui        screen model, screens, overlays, touch router, intro animations
net       wifi, captive portal, http routes, mqtt commands, ota, mdns, sntp
main      app_main and the hook table, nothing else
```

Two deliberate departures from a strict bottom-up order, both for
testability:

- Wi-Fi and a minimal `/api/v2` surface arrive in Phase 1 (D16), so that
  every later milestone can be driven and inspected over HTTP.
- The on-device UI arrives before the remote sources (D17), so that sources
  have a message surface and Makapix has its registration screen.

## How a milestone runs

1. Read its entry here and the review sections it cites.
2. For each file it takes from `legacy/`: check `docs/known-issues.md` for
   that file, move it with `git mv`, strip upward includes, fix or re-record
   its known issues, route allocations, tasks and errors through `util`.
3. Write the tests the entry names. Logic gets host tests before it runs on
   the device.
4. Build both configurations, run the host tests, flash the test build, run
   the device tests.
5. Record anything only Fab can check on the manual checklist.
6. Checkpoint (ledger, decisions, parity, signed commit, push).

A milestone is done when:

- its exit criteria below are met and the ledger says at which tier;
- CI is green on the pushed commit;
- the dev unit boots the result;
- nothing it moved still exists in `legacy/`;
- `AGENTS.md` on `v2` matches the tree.

A milestone that needs design detail beyond a line in
[DECISIONS.md](DECISIONS.md) gets one page in `docs/2.0/design/`, linked from
its entry here. Most will not need one.

## Phase 0: tooling

Everything the build needs before the first line of 2.0 firmware matters.

### M0.1 Tree reset and empty app

- Move `components/`, `main/`, `webui/` and the 1.x build files into
  `legacy/` with `git mv` so history follows.
- Delete outright, never to be moved: PICO-8 (`components/pico8`,
  `http_api_pico8.c`, `webui/pico8/`, `playback_controller.c`), the dead code
  of finding 23, the three forwarding services (finding 13). Record the
  deletion list in the ledger.
- New root project: `main/` with `app_main` only, an empty `components/`,
  version `2.0.0-dev`, the D1 partition table, a `sdkconfig.defaults` seeded from the 1.x configuration with
  the silicon-revision guard intact (D21).
- Erase the dev unit and flash the empty app. The 1.x flash backup already
  exists (see the ledger).
- Rewrite the architecture half of `AGENTS.md` on `v2` for the new tree.
- Closes: findings 13, 23; starts finding 1, 15, 21.
- Exit: both configurations build; the dev unit boots and prints its version
  on UART; nothing outside `legacy/` references 1.x code.

### M0.2 Host test harness

- `tests/host/`: CMake project, Unity, CTest, a FreeRTOS stub that grows on
  demand. Builds with MinGW gcc on the workstation and gcc on Linux, where
  it also runs under AddressSanitizer and UBSan.
- One real test proves the wiring.
- Closes: the host half of finding 22.
- Exit: `ctest` passes locally and in WSL.

### M0.3 Continuous integration

- A GitHub Actions workflow on pushes to `v2`: host tests; firmware build in
  the release and test configurations inside the `espressif/idf:v5.5.4`
  image; lints (layer rule from each component's `REQUIRES`, forbidden
  patterns `weak`, `WHOLE_ARCHIVE`, includes from `main/` or `legacy/`); the
  silicon-revision guard; `p3a` image at most 90 percent of its slot.
- Closes: finding 22, audit H19.
- Exit: a pushed commit goes green, and a deliberately broken one goes red
  for each check.

### M0.4 Device lab

- `host/p3a-lab/`: build (with the guards of `host/jitter-lab/build.ps1`),
  flash, erase, serial capture without reset, secrets loading and
  redaction, wait for boot. The console provision command arrives with
  M1.4 and M1.5, when there is somewhere to store what it sets.
- `sdkconfig.test.defaults` with `CONFIG_P3A_TEST_HOOKS` and the UART test
  console (D20).
- `tests/device/`: pytest fixtures (target guard, per-test serial capture,
  panic and watchdog detection) and the first smoke test.
- Exit: one command takes the unit from erased to booted and reports the
  version; the smoke test passes.

## Phase 1: platform

The device boots, mounts the card, holds settings, joins Wi-Fi and answers
over HTTP.

### M1.1 util

- Allocation helpers with PSRAM as the default and internal RAM opt-in;
  `p3a_limits.h`; the project error base (`NOT_PLAYABLE`, `CORRUPT`,
  `MISSING`, `RATE_LIMITED`, `AUTH`, `NO_KEY`, `SD_FAILED`); the task helper
  with classes `RENDER`, `TOUCH`, `APP`, `NETWORK`, `BACKGROUND`; two job
  runners (I/O and UI); the event bus with its purpose written down (O8) and
  handlers called outside the dispatch mutex; the DMA-pool watermark.
- Closes: findings 9, 12, 14, the signal half of 10; audit M9.
- Tests: host tests for everything that is not a thin IDF wrapper.
- Exit: host tests pass; the empty app creates its tasks through the helper.
- As built: `p3a_limits.h` waits for its first constant; the event bus was
  rewritten rather than moved (D22).

### M1.2 board

- From `legacy/`: `p3a_board_ep44b`, the LCD and touch drivers, SD mount
  with the aligned FatFS glue, `sdio_bus`, `sd_idle_wait`, brightness, the
  BOOT button, `slave_ota`. The `p3a_core` to board to `slave_ota` cycle is
  cut here.
- Audit H5, M28.
- Tests: device tests through the UART console: test pattern on the panel,
  touch controller present, card mounts and reports its size, co-processor
  version read and flashed when stale.
- Manual: panel looks right, touch responds to a finger.
- Exit: device tests pass; boot reaches a test pattern.
- As built: four components (`p3a_board`, `sdio_bus`, `sd_idle_wait`,
  `slave_ota`) plus `frame_trace`, which moved early into `util` because
  `sd_idle_wait` records marks through it. The touch-failure recovery and
  the co-processor update screen wait for settings and the UI.

### M1.3 storage

- From `legacy/`: `sd_path`, `fs_atomic`, `sd_health`, `sd_repair`. New: the
  framed container (`magic, schema, version, record_size, count, flags,
  crc32`) with one loader, one saver and one rebuild-on-mismatch rule; the
  FAT assumptions stated once; a cached free-space figure.
- Closes: finding 5 (the container itself), part of 18.
- Tests: host tests for the container codec and for `sd_path`; device tests
  for atomic write, the health latch and a directory repair.
- Manual: behaviour across a power cut during a write.
- Exit: host and device tests pass.
- As built: one component, `p3a_storage`. The per-store paths of the 1.x
  layout were dropped from `sd_path`; M2.2 defines 2.0's. `sd_repair`'s
  repair paths are carried over untested (they need a failing card).

### M1.4 settings

- One schema file, `{key, type, default, min, max, reboot_required, group,
  label}`. The build generates the C table and typed struct. The store reads
  NVS once, holds the struct behind a mutex, writes one blob on change.
  Unknown keys are rejected. Wi-Fi and Makapix secrets keep their own
  namespaces. Factory reset. The Kconfig, settings and constants rule is
  written into `AGENTS.md`.
- Closes: finding 6 (firmware side), 21; audit M5, M12, M20, M22.
- Tests: host tests for the generator, validation, defaults and the blob
  round trip; device test that a setting survives a reboot.
- Exit: host and device tests pass.
- As built: the schema is `components/p3a_settings/settings.json` and the
  generator `tools/gen_settings.py`. The blob is JSON of the non-default
  values (D31). The component also holds `p3a_recovery`, the counted
  restart rule. The 1.x `config_store` was not moved; it is replaced.

### M1.5 net base

- From `legacy/`: `wifi_manager` with the captive portal, mDNS, SNTP,
  connectivity tracking. New: the static table for embedded, gzipped pages
  (the portal pages are its first users), the HTTP route table and matcher,
  `send_ok` and the error envelope, `GET /api/v2/status`, `/api/v2/config`
  validated by the schema, `/api/v2/jobs/{id}`, reboot. HTTP test hooks:
  screenshot, clock, log capture.
- Closes: findings 16, 17, the job half of 18; audit H6, H16.
- Tests: host tests for the matcher and envelope; device tests: provision
  over UART, join Wi-Fi, status and config over HTTP, a screenshot comes
  back.
- Manual: the captive portal from a phone.
- Exit: from erased flash to answering `/api/v2/status` with no human step.

## Phase 2: player and library, local content

The device plays the user's own files through the complete 2.0 content loop.

### M2.1 Display pipeline and decoders

- From `legacy/`: the display renderer (triple buffer, vsync), the
  upscalers, the animation player's decode path, `animation_decoder`,
  `animated_gif_decoder`, `libwebp_decoder`, the libpng fork, `frame_trace`,
  `mem_stats`. The jitter fixes move untouched.
- Audit H1, H14, M10.
- Tests: a corpus of small files per format in `tests/assets/`; device tests
  play each through a test hook and compare screenshots with stored
  references; a short frame-trace baseline.
- Manual: playback looks smooth; colours and transparency look right.
- Exit: every format in the corpus renders to its reference.

### M2.2 Library core

- `record_t` (192 bytes, discriminator in the record, 96-byte identifiers),
  `index_t` (slot table, availability bitmap), the blob store keyed by
  content hash. (The `refs` index was dropped, D48; the set of pinned
  blobs arrives with the pin lists.)
- Closes: findings 3, 20, the store half of 4; change B.
- Tests: host tests for layout, codec, slot table at 4096 records, bitmap,
  reference counting, recovery from a torn `refs` write.
- Exit: host tests pass; an index round-trips on the card.

### M2.3 Source interface, local source, scheduler

- `source_ops_t` and its registry; the local source (scan and upload); the
  canonical packed playset, playset store and active playset; SWRR with
  credit rollback; one pick function; history; at most 16 resident channel
  indices; `player_commands`. Routes: playsets, actions, upload.
- Closes: finding 2 (interface and first source); audit M21, M7.
- Tests: host tests for SWRR, pick, playset codec and JSON, channel id;
  device tests for upload and playset activation.
- Exit: host and device tests pass.

### M2.4 Loader pipeline, C and D

- The chain `swap_intent`, `swap_ready`, `swap_shown`; history, last-played
  and the view tracker hang off `swap_shown` only. Failure routing by the
  error enum, the loud and silent split, the dwell timer, pause, background
  colour, rotation, the upscale policy from the source table, the decode
  budget (O10).
- Change C: the repetition ring and the weighted pick. Change D: the aspect
  predicate.
- Closes: finding 7; changes C and D (D is seen on the device in M4.3).
- Tests: host tests for the pipeline against a fake loader and renderer, for
  the pick distribution under C, for the D predicate; device tests: a
  playset of local files in both pick modes, next, back, pause, a corrupt
  file is skipped.
- Exit: the unit plays local files under a playset, driven over the API.

## Phase 3: on-device UI

### M3.1 Screen model and hook table

- One model of what is on screen (O7); the hook table `main` fills; the
  channel-message surface.
- Closes: finding 11; audit M25, M27.
- Tests: host tests for the model's transitions.

### M3.2 Text and fonts

- The text path (O3), the chosen fonts (O4), image assets through
  `EMBED_FILES`.
- Closes: finding 24; idea 3.
- Tests: device screenshot references for each font and size.
- Manual: Fab approves the look.

### M3.3 Screens

- From `legacy/`: the info screen, channel messages, fatal error, the
  registration code, setup, the SD error and format flow, the repair notice,
  the boot logo and the 22 intro animations with `host/intro-anim-lab`
  still working.
- Tests: device screenshot references per screen.
- Manual: Fab reviews each screen once.

### M3.4 Touch router and gestures

- A table keyed on the screen model: tap halves, swipe up and down, two
  finger rotate, long press, the rescue gestures, the BOOT button. A touch
  injection hook.
- Tests: device tests drive every gesture through injection.
- Manual: real fingers.

### M3.5 Overlays

- From `legacy/`: FPS, processing, reaction, pin. New: clock (idea 1, O5),
  source branding (idea 2, O6).
- Tests: device screenshot references; host tests for the branding band
  geometry.

### M3.6 USB mass storage

- The composite device, the always-reboot rule, the USB indicator.
- Manual: mount from a computer, copy a file, eject, confirm it plays.

## Phase 4: remote sources

### M4.1 Fetch, download, refresh, eviction

- From `legacy/`: `http_fetch` with the TLS gate, hashing into the blob
  store as bytes arrive; `storage_eviction`, now told about pins by data.
  New: one download task, one refresh task, `refresh_state_t`, back-off on
  the memory watermark and on a full card. The internal-RAM measures (O9).
- Closes: the consumer half of finding 10, the download half of 4.
- Tests: host tests for the refresh state machine and back-off; device
  tests arrive with the first source.

### M4.2 Giphy and Klipy

- Both behind `source_ops_t`, with their settings, cooldown, auth and
  no-key states. Branding is seen on the device here.
- Tests: host tests for the listing parsers on recorded responses; live
  device tests with the keys from the secrets file.

### M4.3 Museums

- The adapters behind `source_ops_t`; `museums.json` as the single table
  compiled into the firmware; the Rijks resolver; the shared rate-limit
  cooldown. The CMA and Wellcome workarounds are deleted. AIC stays marked
  unavailable. D is checked on the device.
- Closes: part of finding 19; audit M13, M14.
- Tests: host tests per adapter on recorded responses; live device tests.

### M4.4 Pinned lists and "show this"

- A pin is a reference, never a copy. Named lists as channels. The single
  artwork slot and `show_url`.
- Closes: the pin half of finding 4; audit H18, M11, M16.
- Tests: host tests for list operations; device tests for pin, unpin, play
  a list, show a URL.

### M4.5 Makapix

- From `legacy/`: MQTT over mTLS, registration, certificate renewal, status
  publishing, the view tracker, reactions. Refresh runs on the content
  task, and asks over HTTPS (D80; this replaced "pages arriving through a
  queue"). MQTT commands call the scheduler's functions (D54).
- Closes: finding 8; audit H4, H13, M26.
- Tests: host tests for page merge and command parsing; live device tests
  against production (D13).
- Manual: Fab registers the unit, sends an artwork and a command from the
  site.

## Phase 5: web UI, OTA, release engineering

### M5.1 Embedded web UI

- The pages ported to `/api/v2` with one `api.js`, scripts split into
  `static/*.js`, the settings page rendered from the schema, the museum
  browse reading `museums.json`, shared helpers. PWA icons shrunk.
- Closes: findings 15, 19, the UI side of 6; change A; audit H11, M6, M18,
  M19, M29.
- Tests: `node --test` for the shared JavaScript; browser-driven checks of
  each page against the dev unit.
- Manual: Fab uses each page once.

### M5.2 Firmware OTA for 2.x

- Check, install as a job, rollback, the renamed asset and compatibility
  field (O1), the hardening choices (O12).
- Audit H8, M23.
- Tests: device test of a full update cycle between two test builds served
  from a local release mirror.
- Manual: one real update from a GitHub pre-release.

### M5.3 Release engineering

- The release folder and manifest without `storage.bin`, the size check,
  the web flasher for 2.x with a clear warning that 2.0 erases a 1.x device,
  `p3a-flasher.exe`, the publish workflow.
- Manual: the web flasher end to end.

### M5.4 Documentation

- `README.md`, `docs/QUICK-START.md`, `docs/HOW-TO-USE.md`,
  `docs/flash-p3a.md`, `docs/infrastructure/`, a 1.x to 2.0 migration page,
  `docs/known-issues.md` and `docs/deferred/` re-triaged, `AGENTS.md` final.

## Phase 6: hardening and release

### M6.1 Soaks

- The jitter pass bar from `docs/jitter/README.md` on a trace build under
  the work mix. The memory-pressure soak under a full refresh cycle,
  including the 64-channel stress that produced the 1.x livelock. A
  multi-day run.

### M6.2 Parity audit

- Every row of [PARITY.md](PARITY.md) is done or explicitly dropped.
  `legacy/` is empty and removed. Closes finding 1.

### M6.3 Field test

- The manual checklist is cleared. A second board goes from the box to
  playing through the public instructions (O14).

### M6.4 Release

- `v2` becomes `main` (O13). Fab cuts 2.0.0.

## Traceability

### Review findings

| # | Finding | Milestones |
|---|---|---|
| 1 | Cyclic components, `main` as a component, weak externs | Rule from M0.1, enforced by M0.3, closed at M6.2 |
| 2 | No source interface | M2.3, then M4.2 to M4.5 |
| 3 | Five record layouts | M2.2 |
| 4 | Five blob stores | M2.2, M4.1, M4.4 |
| 5 | Nine index formats | M1.3, instances in M2.2, M2.3, M4.4 |
| 6 | Settings without a schema | M1.4, M5.1 |
| 7 | History committed before load | M2.4 |
| 8 | Makapix refresh five layers deep | M4.5 |
| 9 | Tasks | M1.1, applied as modules move |
| 10 | Allocation idioms, no pressure signal | M1.1, M4.1, M6.1 |
| 11 | Seven copies of screen state | M3.1 |
| 12 | Event bus as a relay | M1.1 |
| 13 | Forwarding services | M0.1 |
| 14 | No error base | M1.1, M2.4 |
| 15 | Web UI partition and OTA | M0.1, M5.1 |
| 16 | Routing | M1.5 |
| 17 | Response envelope | M1.5 |
| 18 | Slow work on the httpd task | M1.3, M1.5, M5.2 |
| 19 | UI helpers, limits, museum tables | M4.3, M5.1 |
| 20 | 32-character identifiers | M2.2, M2.3 |
| 21 | Kconfig | M0.1, M1.4 |
| 22 | No tests, no CI | M0.2, M0.3 |
| 23 | Dead code | M0.1 |
| 24 | Hex image arrays | M3.2 |
| 25 | Lock-order inversion | Fixed in 1.x on `main` (D9). In 2.0 the lifecycle lock does not exist; confirm at M4.1 |

### Announced changes and ideas

| Item | Milestones |
|---|---|
| A, single binary | M0.1 (partitions), M1.5 (embedding), M5.1 |
| B, 192-byte records | M2.2 |
| C, suppress repetitions | M2.4 |
| D, aspect-ratio filter | M2.4, seen on the device at M4.3 |
| Idea 1, clock overlay | M3.5 |
| Idea 2, Giphy and Klipy branding | M3.5, seen on the device at M4.2 |
| Idea 3, VEXED fonts | M3.2 |

### Known issues and audit items

Each is handled when its code moves. "Gone" means the code that had the
problem is not carried into 2.0; the milestone confirms nothing equivalent
was reintroduced.

| Item | Where |
|---|---|
| SDIO RX internal-RAM exhaustion | M1.1 (signal), M4.1 (measures, O9), M6.1 (soak) |
| Slow software JPEG decodes | M2.4 (O10) |
| H1, H14, M10 | M2.1 |
| H2, H3, M1, M2, M3 | Gone with the cache and picker rewrite; M2.2, M2.3 |
| H4, H13, M26 | M4.5 |
| H5, M28 | M1.2 |
| H6, H16 | M1.5 |
| H8, M23 | M5.2 |
| H9, M15 | Gone with change A |
| H10 | M0.1 onwards: `app_main` degrades instead of aborting |
| H11, M6, M18, M19, M29 | M5.1 |
| H12, M8 | Gone with PICO-8 |
| H18, M11, M16 | M4.4 |
| H19 | M0.3 |
| M4 | Gone with `playlist_manager` |
| M5, M12, M20, M22 | M1.4 |
| M7, M21 | M2.3 |
| M9 | M1.1 |
| M13, M14 | M4.3 |
| M24 | M6.1, documented in M5.4 |
| M25, M27 | M3.1 |
| Low-severity lists | Checked file by file as each file moves |
