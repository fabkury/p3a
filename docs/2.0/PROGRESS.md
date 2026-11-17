# 2.0 progress ledger

Part of the [2.0 work stream](README.md). This is the first page to read in
any session and the last to update in any checkpoint. Keep "Now" true at all
times.

## Now

| | |
|---|---|
| Phase | 1, platform (Phase 0 complete) |
| Current milestone | M1.2 board |
| Next action | Continue M1.2. Done: panel, framebuffers, backlight, BOOT button. Remaining, in order: (1) touch: move the I2C recovery and init parked in `legacy/components/p3a_board_ep44b/p3a_board_touch_init.c.part` and the driver half of `legacy/main/app_touch.c` (audit H5), with a `touch` console probe; (2) SD card: mount with the aligned FatFS glue (`legacy/main/ffsystem_aligned.c`), `sdio_bus`, `sd_idle_wait` (it requires `frame_trace` and uses a weak reference, which the linter forbids: decide how); (3) co-processor: esp_hosted (un-park its three sdkconfig lines, mind `ESP_IDF_VERSION=5.5`), `slave_ota` and its `slave_fw` flashing, without its dependency on `p3a_core` |
| Blocked on | Nothing |
| In flight | Nothing |
| Uncommitted work | None |

## Dev unit

| | |
|---|---|
| Running | `2.0.0-dev`, test configuration (`build-test`), flashed 2026-11-17 by the device tests. Left showing colour bars |
| Flash state | Bootloader, 2.x partition table, app in `ota_0`. NVS empty. `slave_fw` empty (the C6 keeps its own firmware, 2.7.0 as last reported by 1.x) |
| SD card | 32 GB, healthy (D8). Still holds 1.x content under `/p3a2`; to be wiped at M0.4 or M1.3 |
| 1.x flash backup | `%USERPROFILE%\.p3a-lab\backups\p3a-fab_v1.2.4_full-flash_2026-11-17.bin` (see the session log for size and hash) |
| Secrets file | Complete: four API keys read from the unit, Wi-Fi password filled in by Fab |

## Milestones

Status is one of: not started, in progress, done (date, commit, tiers),
reopened (why).

| Milestone | Status |
|---|---|
| M0.1 Tree reset and empty app | done 2026-11-17. Tiers: local release build; flashed, boot log read over UART. No CI yet (M0.3), no test configuration yet (M0.4) |
| M0.2 Host test harness | done 2026-11-17. Tiers: host tests pass with MinGW gcc 16.1 and with gcc 13.3 under ASan and UBSan in WSL; firmware builds with the new component |
| M0.3 Continuous integration | done 2026-11-17. Tiers: CI green on GitHub for tree checks, host tests and the firmware build; each linter rule proven to fail by its self-test (29 cases). No deliberately broken commit was pushed |
| M0.4 Device lab | done 2026-11-17. Tiers: device tests (6 cases, run four times), lab unit tests (4 cases), both builds checked. Provisioning waits for M1.4 and M1.5 |
| M1.1 util | done 2026-11-17. Tiers: host tests (6 programs, under ASan and UBSan too), device tests (21 cases, run twice), both builds checked, CI. `p3a_limits.h` is not created yet: it arrives with its first constant |
| M1.2 board | in progress: display, backlight and button done 2026-11-17 (31 device tests, both builds). Touch, SD card and co-processor remain |
| M1.3 storage | not started |
| M1.4 settings | not started |
| M1.5 net base | not started |
| M2.1 Display pipeline and decoders | not started |
| M2.2 Library core | not started |
| M2.3 Source interface, local source, scheduler | not started |
| M2.4 Loader pipeline, C and D | not started |
| M3.1 Screen model and hook table | not started |
| M3.2 Text and fonts | not started |
| M3.3 Screens | not started |
| M3.4 Touch router and gestures | not started |
| M3.5 Overlays | not started |
| M3.6 USB mass storage | not started |
| M4.1 Fetch, download, refresh, eviction | not started |
| M4.2 Giphy and Klipy | not started |
| M4.3 Museums | not started |
| M4.4 Pinned lists and "show this" | not started |
| M4.5 Makapix | not started |
| M5.1 Embedded web UI | not started |
| M5.2 Firmware OTA for 2.x | not started |
| M5.3 Release engineering | not started |
| M5.4 Documentation | not started |
| M6.1 Soaks | not started |
| M6.2 Parity audit | not started |
| M6.3 Field test | not started |
| M6.4 Release | not started |

## Manual checklist

Checks only Fab can do (D10). Each line: id (milestone plus a letter), what
to do, what should happen. Fab ticks a line or reports what he saw; a failed
check reopens its milestone.

Open:

- **M1.2-a, the panel shows what the framebuffer holds.** Run
  `python host/p3a-lab/lab.py cmd "lcd bars"`. The panel should show eight
  vertical bars of equal width, left to right: white, yellow, cyan, green,
  magenta, red, blue, black. Edges sharp, colours pure, no flicker or
  tearing. Then `python host/p3a-lab/lab.py cmd "lcd gradient"`: a smooth
  gradient with a thin white border on all four edges and a red square in
  the top-left corner (top-left as the board is normally held, USB ports at
  the bottom).
- **M1.2-b, the backlight dims.** Run
  `python host/p3a-lab/lab.py cmd "lcd brightness 20"`: clearly dimmer.
  `... "lcd brightness 0"`: backlight off. `... "lcd brightness 100"`: full
  again.
- **M1.2-c, the BOOT button is seen.** Run
  `python host/p3a-lab/lab.py cmd button` (prints `"presses": 0` or the
  count so far), press BOOT (the top button) twice, run it again: the count
  is two higher.

Cleared:

- None yet.

## Open questions for Fab

Decisions that are still open, with the milestone that needs them. Details
and proposals are in [DECISIONS.md](DECISIONS.md) under "Open decisions".

| Id | Question | Needed before |
|---|---|---|
| Q1 to Q10 | Ten 1.x behaviours that look accidental (brightness not restored at boot, API keys readable over the LAN, no update check for 12 hours, and others). Keep or change each? Listed in [PARITY.md](PARITY.md) under "Behaviours to decide" | M1.3 at the earliest; each row names its milestone |
| O2 | Should 1.2.5 carry a notice about 2.0, and with what wording? | 1.2.5 |
| O4 | Which VEXED font families and sizes? | M3.2 |
| O3 | Keep uGFX for text, or replace it with a glyph-sheet renderer? | M3.2 |
| O5 | Clock overlay details (12 or 24 hour, seconds, when hidden) | M3.5 |
| O6 | Branding overlay rules from the Giphy and Klipy terms | M3.5 |
| O15 | Does Makapix need any server-side change for 2.0 devices? | M4.5 |
| O1 | Exact name of the 2.x firmware asset | M5.2 |
| O12 | OTA hardening choices | M5.2 |
| O14 | A second board for the field test | M6.3 |
| O13 | How `v2` becomes `main` | M6.4 |

The remaining open decisions (O7, O9, O10) may be taken as working decisions
by the agent and are reported when taken.

## Ports from main

Fixes made on `main` after `v2` branched (at `4c9de368`) that 2.0 must pick
up by hand. Most 1.x fixes will not apply because the code is being
rebuilt; list them anyway and mark each "ported", "not applicable" with the
reason, or "pending".

| Commit on main | What | Status in 2.0 |
|---|---|---|
| (none yet) | | |

Expected: the lock-order fix for finding 25 (D9). In 2.0 the lifecycle lock
it concerns should not exist; confirm at M4.1 and mark it not applicable.

## Deletion log

What was deleted from `legacy/` without being moved, and why. Filled from
M0.1 on.

M0.1, 2026-11-17 (paths relative to the 1.x tree):

- PICO-8 (decision C5): `components/pico8/`, `webui/pico8/`,
  `components/http_api/http_api_pico8.c` and `pico8_logo_data.h`,
  `main/playback_controller.c` and its header,
  `scripts/convert_pico8_logo.py`.
- Dead code (finding 23): `components/content_cache/`,
  `components/playback_queue/`; in `channel_manager`: `channel_interface.h`,
  `sdcard_channel_impl.c/.h`, `playlist_manager.c/.h`,
  `channel_settings.c/.h`; in `makapix`: `makapix_channel_switch.c`,
  `makapix_single_artwork.c`. `makapix_channel_switch.c` also held the live
  `makapix_show_artwork`, a thin call into the scheduler; M4.5 writes its
  equivalent.
- Forwarding services (finding 13): `main/connectivity_service.c`,
  `content_service.c`, `render_engine.c` and their headers.
- Web UI update stack (change A):
  `components/ota_manager/ota_manager_webui.c`,
  `components/http_api/surrogate_ui.h`, `webui/static/compat.js` and
  `compat.js.in`, `webui/metadata.json`.

Dead functions inside files that are otherwise live were not hunted down;
they are simply not moved when their file is.

M1.2, 2026-11-17:

- `components/p3a_board_ep44b/p3a_board_fs.c`: the LittleFS mount and the
  web UI health check. Gone with change A.
- `components/p3a_board_ep44b/CMakeLists.txt` and `Kconfig`: replaced by
  the new component's. The Kconfig held the pixel-format choice, the
  capability flags (`P3A_HAS_*`) and the button GPIO; 2.0 targets one board
  with one format, so none of them is an option any more (audit M28).

M1.1, 2026-11-17:

- `components/event_bus/` (342 lines). Rewritten as `p3a_event` in
  `components/p3a_util` rather than moved: the 2.0 bus drops categories,
  runs handlers outside its lock, counts dropped events, and takes its
  event types from a new list that starts almost empty. The 18 event types
  of 1.x are not carried over; each returns only if its emitter arrives and
  it is a real fan-out (D22).

## Session log

Newest first. One entry per session or checkpoint: date, what was done,
what was verified and at which tier, the commit.

### 2026-11-17: M1.2 first step, panel, backlight, button

- CI on the M1.1 commit (`f544925f`): all four jobs green.
- New component `components/p3a_board` (layer `board`).
  `p3a_board_display.c`, `p3a_board_button.c` and `p3a_board.h` moved from
  `legacy/components/p3a_board_ep44b`. The header is rewritten for one
  board and one pixel format: no capability flags, no RGB565 path, no
  redefinition of Kconfig symbols (audit M28).
- The BOOT button no longer emits an event or knows about the co-processor
  update: the board reports a debounced press to one callback, and what a
  press means is the caller's business. One listener, so not an event (D22).
- Dependencies: the Waveshare BSP and everything it pulls in are declared
  in `components/p3a_board/idf_component.yml`, pinned to the exact versions
  1.x shipped with. `dependencies.lock` at the root is tracked. LVGL comes
  with the BSP and is not used; dropping the BSP for the bare panel driver
  would remove it, which is an idea for `ideas.md`, not part of this work.
- `sdkconfig.defaults`: the three BSP display lines are live; the
  esp_hosted lines stay parked for the co-processor step.
- `app_main` brings the display up painted black. The background colour
  will come from the settings store (M1.4).
- Test hooks: `p3a_test_board.c` with `lcd` (info, fill, bars, gradient,
  pixel, crc, shot, brightness) and `button`. `lab.py shot <file>` saves a
  downscaled PNG of the framebuffer over the UART, so the agent can look at
  what was drawn before the HTTP screenshot hook exists (M1.5).
- Found by the tests: a 180x180 screenshot always had one short row. The
  first theory, bytes lost in the host's serial buffer, was wrong. The
  console task printed for more than 15 seconds without yielding, the task
  watchdog reported the starved idle task, and that report landed in the
  middle of a row. The screenshot command now yields once per row. The lab
  changes made while chasing the wrong theory (a 1 MB receive buffer, log
  flushed when idle instead of per line) are kept; they are sound, but
  they were not the fix.
- The agent looked at two screenshots (gradient at 360x360, bars at
  180x180): both correct.
- Memory with the board up: internal DMA pool 434 KB total, 378 KB free,
  largest block 254 KB.
- Image size: release 286 KB, test 367 KB.
- Verified, device tests: 31 pass after a flash and again without one.
  Framebuffer contents are checked by pixel probes and by a CRC of the
  whole frame against a value computed on the host. Verified, build
  checks: release and test. Awaiting manual checks M1.2-a, b and c: the
  tests read the framebuffer, not the glass, and nothing can press the
  button.

### 2026-11-17: M1.1 complete, jobs, events, memory pressure

- CI on the first M1.1 commit (`8d00fe38`): all four jobs green, including
  the repaired test-configuration build.
- Jobs: `p3a_job_table` (pure: ids, states, replacement of the oldest
  finished record) and `p3a_job` (two runner tasks, I/O and UI, class APP,
  12 KB PSRAM stacks, eight queued jobs each). `p3a_job_set_progress()`
  works from inside a job. This is what replaces the twelve one-shot tasks
  of 1.x and what `/api/v2/jobs/{id}` will read at M1.5.
- Events: `p3a_event` with the rule for what the bus is for written at the
  top of its header (decision D22, closes O8). Handlers run with no lock
  held (audit M9); a full queue drops, counts and logs. Subscriber table is
  pure and host-tested.
- Memory: `p3a_mem`. The level decision (OK, LOW, CRITICAL, with a recovery
  margin against flapping) is pure and host-tested; the device half samples
  the internal DMA pool on demand, at most every 250 ms, and announces a
  change as an event. No task and no timer. Thresholds are provisional
  (O9). The allocation-failure heap report moved in from
  `legacy/main/p3a_main.c`, still one per boot.
- `app_main` gained `boot_step()`: a failed step is logged and boot carries
  on (audit H10's direction).
- Test console split into a core and one command file per area
  (`p3a_test_util.c`), with commands `task`, `errname`, `job`, `jobinfo`,
  `event`, `mem`.
- Found by the tests:
  - Host: the recovery margin was computed in 64 bits and then truncated to
    32. Unreachable with real thresholds, fixed anyway.
  - Device: a flash write (NVS set and commit) from a job runner whose stack
    is in PSRAM works on this chip. This was a real question; the runners
    would otherwise have needed internal-RAM stacks.
  - Device: with the near-empty 2.0 firmware the internal DMA pool is
    447 KB with about 400 KB free and a largest block of 254 KB. Fully
    loaded 1.x left about 272 KB. Watch this number shrink as components
    arrive; it is the budget the known SDIO issue is about.
  - Device: exhausting the pool with 2 KB chunks leaves 15 KB free with a
    largest block of 1.4 KB. That is the fragmentation case the "largest
    block" threshold exists for.
  - Two failures were wrong assumptions in the tests themselves (pool size,
    a hog limit in the test hook), not firmware faults.
- Verified, host tests: 6 programs (version, err, task policy, job table,
  event table, mem pressure) pass with MinGW and under ASan and UBSan.
  Verified, device tests: 21 pass, twice in a row, including job ordering
  across runners, a full job queue, event fan-out of 200 through a 32-slot
  queue, a deliberate event flood with drops reported, pressure levels
  following a hogged pool with one event per change, and exactly one heap
  report per boot. Verified, build checks: release and test.
- Not done: `p3a_limits.h`. It has nothing to hold yet; the milestone that
  needs the first shared constant creates it.

### 2026-11-17: M1.1 first slice, allocation, errors, tasks

- CI on the M0.4 commit: tree checks, host tests and the release build
  green; the new test-configuration job failed on an unquoted `;` in its
  own arguments. Fixed in this commit.
- `psram_alloc.h` moved unchanged from `legacy/components/channel_manager`
  to `components/p3a_util`. It keeps its name and API so that code moving
  out of `legacy/` later needs no edits for it. No host test: it is three
  inline wrappers over `heap_caps_*`.
- `p3a_err`: the error base at `0x50000` with `NOT_PLAYABLE`, `CORRUPT`,
  `MISSING`, `RATE_LIMITED`, `AUTH`, `NO_KEY`, `SD_FAILED`, stable names,
  and `p3a_err_to_name()` for any `esp_err_t`. A code added without a name
  fails to compile. First host stub: `tests/host/stubs/esp_err.h`.
- `p3a_task_policy` (pure) and `p3a_task` (FreeRTOS): five classes carrying
  priority, core and stack memory; `p3a_task_create()` is the only way to
  make a task. It uses the IDF's `xTaskCreatePinnedToCoreWithCaps` for
  PSRAM stacks, with a fallback to internal RAM. Tasks made this way are
  long-lived by rule; run-once work will go to the job runners.
- Policy choices, taken from what 1.x shipped rather than from the review
  text: render 5 on core 1 with internal stacks; touch 5, unpinned, PSRAM
  stack (the review says internal, but 1.x deliberately put the touch stack
  in PSRAM to spare the DMA pool); app 4, network 3, background 2, all on
  core 0 with PSRAM stacks. A host test pins these values so that changing
  one is a visible decision.
- The test console now creates its task through the helper and gained two
  commands, `task` and `errname`.
- Verified, host tests: 3 executables (version, err, task policy) pass with
  MinGW and under ASan and UBSan in WSL. Verified, device tests: 8 pass
  after a test build and flash; the console task reports priority 2, core
  0, stack in PSRAM, and error names resolve on the device. Verified, build
  checks: release and test builds pass `check_tree.py --build`.

### 2026-11-17: M0.3 and M0.4, CI and the device lab

M0.3:

- `tools/check_tree.py`: the structural rules as a script (layer rule from
  `components/LAYERS.txt` and each `REQUIRES`, no cycles, nothing requires
  `main`, no weak symbols, no `WHOLE_ARCHIVE`, no includes from `main/` or
  `legacy/`, the guard lines in `sdkconfig.defaults`; with `--build`, the
  generated revision setting, image at most 90 percent of the slot, test
  hooks only in a test build). Same-layer requirements are allowed; cycles
  are not.
- `tools/test_check_tree.py`: 29 cases, one or more per rule, each building
  a small tree that breaks the rule and expecting the failure. This is how
  "each check can go red" was shown; no broken commit was pushed to `v2`.
- `.github/workflows/v2.yml`: tree checks, lab unit tests, host tests under
  ASan and UBSan, firmware builds for release and test in
  `espressif/idf:v5.5.4`, binaries kept 14 days.
- Verified, CI: the first run (`f917b4a6`, release build only) was green on
  all three jobs. This checkpoint adds the test-configuration build and the
  lab unit tests to CI; check `gh run list --branch v2` for that run.

M0.4:

- `components/p3a_test_hooks` (layer `net`, compiled only with
  `CONFIG_P3A_TEST_HOOKS`): a UART console without echo. Protocol in its
  header: `!command [args]` in, `TEST|ok|json`, `TEST|err|message` or
  `TEST|ready|json` out. Commands: `version`, `reboot`.
- `sdkconfig.test.defaults` and the `build-test/` directory.
- `host/p3a-lab/lab.py`: build with checks, flash, erase, a UART class that
  attaches without resetting, logs every line, redacts secrets, detects
  crashes, and speaks the console protocol; `bring_up()` refuses a board
  that is not the dev unit or runs another version. `README.md` beside it.
- `tests/device/`: fixtures (session bring-up, per-test crash watch,
  `--no-flash`, the `live` marker) and six smoke tests.
- Changed from the plan: the `provision` console command is not here. There
  is nowhere to store credentials until the settings store (M1.4) and Wi-Fi
  (M1.5) exist. The lab half (secrets loading and redaction) is done.
- Found by the tests: the first command after a reset was lost once, in the
  first full run. Most likely a stray byte left in the device receive buffer
  by the reset, glued to the front of the line. The lab now sends an empty
  line after the ready message. Four clean runs since; the cause is not
  proven.
- Found while building the lab: `idf.py` refuses to run under an MSYS
  environment (Git Bash) and still exits 0, so a wrapper sees a "successful"
  build with no output. The lab strips the `MSYSTEM` variables.
- Verified, device tests: 6 pass after a full test build and flash, and 3
  more times with `--no-flash`. Verified, build check: the release map has
  no `p3a_test_` symbol; the test map has 53.
- Design point for M2.2, noted while writing the layer table: `sources` sits
  below `library`, yet both need `record_t` and `source_ops_t`. Those types
  need a home at or below `sources`; decide where when M2.2 defines them.

### 2026-11-17: M0.2 host test harness

- `tests/host/`: CMake project, Unity 2.x vendored unmodified from the IDF
  tree, `p3a_host_test()` helper, warnings as errors, sanitizers on by
  default off Windows. Conventions in `tests/host/README.md`.
- First 2.0 component, `components/p3a_util`, with its first unit:
  `p3a_version` (parse and order `MAJOR.MINOR.PATCH[-suffix]`). It is the
  real code that proves the wiring, and OTA needs it at M5.2. Unlike the 1.x
  comparison, a string that fails to parse has no version instead of
  comparing as equal, and a release orders above its own pre-releases.
- No FreeRTOS stub yet: nothing needs one. `tests/host/stubs/` is created
  with the first stand-in, most likely at M1.1.
- Installed `cmake` and `ninja-build` in the WSL Ubuntu so the sanitizer run
  works locally.
- Verified, host tests: 7 cases pass with MinGW and in WSL under ASan and
  UBSan. Verified, local release build: firmware builds with `p3a_util`.
  Not flashed; the app does not use the unit yet.

### 2026-11-17: M0.1 tree reset and empty app

- Fab filled in the Wi-Fi password, approved pushing `main` (pushed), and
  gave the go.
- Moved the 1.x tree to `legacy/` with `git mv` (`components/`, `main/`,
  `webui/`, the root build files, the sdkconfig files, `flasher/`), and the
  untracked `managed_components/` with it. Deleted what never moves; see the
  deletion log.
- New root project: `CMakeLists.txt` (version `2.0.0-dev`), `main/p3a_main.c`
  (prints version and chip revision), the D1 `partitions.csv`,
  `sdkconfig.defaults`.
- Decision D21 (closes O11): the configuration is `sdkconfig.defaults`, and
  `sdkconfig` is generated and untracked. Found while doing it: a tracked
  full `sdkconfig` in a tree without esp_hosted would silently lose the
  esp_hosted and BSP settings on its first regeneration, and they would come
  back as defaults at M1.2. `idf.py save-defconfig` on the 1.x project
  reduced its configuration to 60 lines; the 2.0 file keeps the IDF-level
  ones and parks the BSP, esp_hosted and TinyUSB ones, commented, under
  their milestones.
- Verified, local release build: builds with no warnings; image 226 KB in a
  10 MB slot; the generated `sdkconfig` has `ESP32P4_REV_MIN_FULL=1`.
- Verified, on the dev unit: erased the flash, flashed, reset, read the UART
  log. The bootloader lists the eight 2.x partitions at the D1 offsets and
  finds 32 MB of PSRAM; the app prints `p3a 2.0.0-dev` and
  `chip rev v1.0, 2 cores`. No errors in the log.
- `AGENTS.md` rewritten for the new tree; `legacy/README.md` added;
  `host/intro-anim-lab/build.ps1` repointed at `legacy/` (not rebuilt).
- Facts for later milestones:
  - The shipped 1.x configuration set `P3A_RENDER_TASK_PRIORITY` and
    `P3A_TOUCH_TASK_PRIORITY` to 5, not the Kconfig defaults of 8 and 6 that
    the review quotes. M1.1 takes the task classes' priorities from what
    shipped.
  - `HTTPD_WS_SUPPORT` was only for PICO-8 and is not carried.
  - The repository is reachable through two paths on this workstation, and
    a build directory configured through one refuses the other. The old
    `build/` had been configured through the non-Dropbox path and was
    deleted.
  - `host/jitter-lab/build.ps1` now builds the 2.0 app and its guards will
    fail; `host/p3a-lab/` replaces it at M0.4 and the lab is adapted at M2.1.

### 2026-11-17: foundations

- Studied the architecture review, the user documents, the lab tooling and
  the dev unit's live status.
- Collected Fab's decisions D2 to D13 in three rounds of questions; recorded
  them with the earlier constraints, the working decisions and the open
  ones in [DECISIONS.md](DECISIONS.md).
- On `main` (`4c9de368`): removed the rule against building, flashing and
  monitoring unasked from `AGENTS.md` and `CLAUDE.md` (D5), and recorded the
  branch policy there.
- Created the `v2` branch and this folder: README, PLAN, PROGRESS,
  DECISIONS, TESTING, PARITY. Pointed `AGENTS.md` on `v2` at it.
- Created `%USERPROFILE%\.p3a-lab\secrets.json` with the four API keys read
  from the dev unit (values never printed).
- Took a full flash backup of the dev unit with esptool: 33,554,432 bytes,
  SHA-256 `e6f393e371d06f40993c209d3964e847b322b9fe752aec53caf2d90f69094363`.
  Its partition table parses as the 1.x layout and `ota_0` starts with a
  valid image header. The unit rebooted into 1.2.4 afterwards and answers on
  the LAN. The read took 9 minutes at 921600 baud.
- Checked the toolchains: ESP-IDF 5.5.4 activates and `idf.py` runs; MinGW
  gcc, CMake, Ninja, CTest, Python with pytest, pyserial, requests and
  Pillow, Node and WSL are present. No firmware build was run.
- Built [PARITY.md](PARITY.md) from the user documents plus a read-only
  agent sweep of the 1.x code. The sweep is unverified line by line; rows
  are re-checked as their code moves. It surfaced ten behaviours to decide
  (Q1 to Q10).
- Finding worth keeping in view: a 1.x unit installs the newest release that
  has an asset named `p3a.bin`, comparing versions only
  (`components/ota_manager/github_ota.c`). This is why D12 exists.
- Nothing was built or flashed. No 2.0 code exists yet.
