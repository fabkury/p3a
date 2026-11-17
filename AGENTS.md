# AGENTS.md

Guidance for AI coding agents working in this repository. This is the canonical
file: `CLAUDE.md` imports it, so project guidance goes here and only
Claude-specific notes go there. It is read at the start of every session, so
keep it accurate and keep it short.

## This branch is p3a 2.0 under construction

`v2` rebuilds p3a on a new architecture. Before doing anything here, read
`docs/2.0/README.md` and then `docs/2.0/PROGRESS.md`: they say where the work
stands, what comes next, and the rules of the work stream. Record progress
there as part of every step.

This file describes the tree as it is on `v2` today and changes in the same
commit as the tree. For 1.x (its architecture, release process and web UI
versioning) read `AGENTS.md` on `main`.

## Project overview

p3a is an ESP32-P4 Wi-Fi pixel art player. It plays WebP, GIF, PNG, APNG, JPEG,
and BMP files, animated and static: GIFs and stickers from Giphy and Klipy,
animated artworks from Makapix Club (a pixel art social network), static museum
artworks over IIIF and similar image CDNs, artworks fetched from a URL, and
local files on the SD card.

Target hardware is the Waveshare ESP32-P4-WIFI6-Touch-LCD-4B: 720x720 IPS
display (24-bit, MIPI-DSI), GT911 capacitive touch, 32 MB PSRAM, 32 MB flash,
and an ESP32-C6 Wi-Fi 6 co-processor reached through esp_hosted over SDIO.

The firmware is public (Apache 2.0). 1.x runs on real users' devices from
`main`; 2.0 has not shipped and runs only on the dev unit.

## Ground rules

- **Build, flash, and monitor whenever it helps.** Compiling to check a
  change, flashing the dev unit, and reading its serial log are ordinary parts
  of the work and need no permission. The dev unit is the board attached to
  the workstation by USB. It belongs to the development work: its flash, NVS,
  and SD card may be erased when the work calls for it. It is the only device
  you flash. Every other unit gets firmware through GitHub Releases, and the
  user cuts those.
- **Say what was tested.** A change that has not run on the device is
  unverified. Report how far each change got and name the tier
  (`docs/2.0/TESTING.md`): host tests, CI build, device tests, soak, manual.
  Do not describe untested work as done, working, or verified.
- **Clarifying questions are always welcome.** Ask when different readings of a
  request would lead to materially different work.

## Branches

- `main` is the 1.x line and is production. It takes critical fixes only. One
  more release is planned, 1.2.5. 2.0 work never touches `main`.
- `v2` is p3a 2.0, a clean break with no OTA path from 1.x (2.0 is installed
  over USB). A fix made on `main` reaches 2.0 only if it is ported by hand;
  the ledger keeps the list.

## The tree

| Path | What |
|------|------|
| `CMakeLists.txt`, `main/`, `components/` | The 2.0 firmware. `main/` holds `app_main` and the boot sequence only. Every component is listed with its layer in `components/LAYERS.txt`. |
| `sdkconfig.test.defaults` | The test overlay: adds the test hooks (`components/p3a_test_hooks`). Test builds go to `build-test/`. |
| `tests/host/` | Host unit tests (Unity, CMake, CTest). |
| `tests/device/` | Device tests (pytest) that flash and drive the dev unit. |
| `tools/check_tree.py` | The structural rules below as a script; CI and the lab run it. |
| `host/p3a-lab/` | Builds, flashes and talks to the dev unit. Use it instead of raw `idf.py` for anything that involves the device. |
| `partitions.csv` | The 2.x partition table, fixed for the life of 2.x (`docs/2.0/DECISIONS.md` D1). |
| `sdkconfig.defaults` | The build configuration and its source of truth. `sdkconfig` is generated and not tracked. |
| `legacy/` | The 1.x sources, kept as the quarry 2.0 is built from. Never compiled, never included, never linked. See `legacy/README.md`. |
| `docs/2.0/` | The work stream: protocol, plan, ledger, decisions, testing, parity. |
| `docs/2.0-architecture-review/` | The design of 2.0. |
| `host/` | Host tooling. `intro-anim-lab` builds from `legacy/` until M3.3. `jitter-lab` targets 1.x endpoints and the 1.x build; it is adapted at M2.1. |
| `docs/` (the rest), `README.md` | Still describe 1.x. They are rewritten in one pass at M5.4; do not patch them piecemeal. |

### Layers

2.0 components belong to layers, and a component may depend only on layers
below its own. Bottom to top: `util`, `board`, `storage`, `sources`,
`library`, `player`, `ui`, `net`, `main`. Every dependency is a `REQUIRES`.
`WHOLE_ARCHIVE`, `__attribute__((weak))`, and including a header from `main/`
or anything from `legacy/` are forbidden. What each layer holds is in
`docs/2.0/PLAN.md`.

### Conventions for 2.0 code

All in `components/p3a_util`; each header explains itself at the top.

- **Tasks** are created only with `p3a_task_create()` and a class
  (`RENDER`, `TOUCH`, `APP`, `NETWORK`, `BACKGROUND`). No literal
  priorities, no hand-rolled PSRAM stacks. Tasks are long-lived.
- **Run-once or slow work** is a job (`p3a_job_submit()`), never a task
  made for one call and never done on a task that must stay responsive.
- **Allocation** defaults to PSRAM through `psram_alloc.h`. Internal RAM is
  asked for explicitly, with the reason in a comment. No plain `malloc` in
  project code.
- **Errors** that a caller must tell apart use `p3a_err.h`
  (`P3A_ERR_NOT_PLAYABLE`, `CORRUPT`, `MISSING`, `RATE_LIMITED`, `AUTH`,
  `NO_KEY`, `SD_FAILED`). Never classify a failure by matching a path or a
  message.
- **Events** (`p3a_event.h`) are for fan-out only. If the emitter knows who
  should act, it calls that code. New event types go in
  `p3a_event_types.h` with their meaning and payload.
- **Optional work** (a download, a refresh, a new TLS session) checks
  `p3a_mem_level()` before starting.
- **Logic is split from hardware** so it can be host-tested: the pure part
  in its own file with no IDF includes (`p3a_job_table.c`,
  `p3a_mem_pressure.c`), the thin device part beside it.
- **Test hooks** live only in `components/p3a_test_hooks`, one command file
  per area, every symbol prefixed `p3a_test_`.

### Moving code out of `legacy/`

Code leaves `legacy/` only inside a milestone: `git mv` it into the new tree,
strip its upward includes, check `docs/known-issues.md` for that file and
fix or re-record what it lists, and land it with tests at the tier the
milestone names. Deleting a legacy file without moving it goes in the
ledger's deletion log with the reason.

## Toolchain and environment

- ESP-IDF v5.5.4, target `esp32p4`, developed on Windows with PowerShell.
  Setting up a new machine: `docs/reference/esp-idf-5.5.4-setup.md`.
- Activate the IDF environment in the same command as the build:

  ```powershell
  $env:PYTHONUTF8 = "1"          # IDF's Python tooling raises Unicode errors on Windows without it
  . <IDF tools root>\Microsoft.v5.5.4.PowerShell_profile.ps1   # EIM install; commonly C:\Espressif\tools\
  $env:ESP_IDF_VERSION = "5.5"   # REQUIRED after activation, see next bullet
  ```

- **`ESP_IDF_VERSION` must be `5.5`, not `5.5.4`.** EIM 0.13.x's profile
  exports the full version, but official IDF 5.5.x activation exports
  major.minor, and `esp_wifi_remote`'s Kconfig keys its version fragments on
  that convention. With the wrong value the fragment silently loads nothing,
  `sdkconfig` is generated with esp_hosted on SPI and an "invalid" slave
  target, and the build dies with `#error "Unknown Slave Target"` deep inside
  esp_hosted. This bites from the milestone that brings esp_hosted back
  (M1.2). Recovery: delete `sdkconfig` and `build/`, set the variable,
  rebuild.
- **Silicon revision guard.** The board's ESP32-P4 is silicon rev v1.0.
  `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` and `CONFIG_ESP32P4_REV_MIN_1=y` in
  `sdkconfig.defaults` must never be removed, and the generated `sdkconfig`
  must show `CONFIG_ESP32P4_REV_MIN_FULL=1`: from IDF 5.5.2 the default is
  rev 3.01 only, and the bootloader rejects such images on this board.
- **`sdkconfig.defaults` is edited, `sdkconfig` is not.** Defaults apply only
  when `sdkconfig` is created, so delete `sdkconfig` after editing the
  defaults file. A setting changed through `menuconfig` is lost unless it is
  copied into `sdkconfig.defaults`. Options for components that 2.0 has not
  brought in yet are parked, commented, at the bottom of the file under the
  milestone that enables them.
- **Run `idf.py` from the repository root**, with the directory change in the
  same command. `idf.py` treats the current directory as the project; run from
  inside a component it configures that component as a standalone project and
  leaves a stray `build/` directory there.
- The repository is also reachable through a second path on this workstation
  (`C:\Users\fab\F\...`). A build directory configured through one path
  refuses the other; always use the Dropbox path.
- The dev unit, its serial port, the secrets file and the 1.x flash backup
  are described in `docs/2.0/README.md` under "Environment".

## Build commands

```powershell
python host/p3a-lab/lab.py build release   # or: build test. Builds and runs the build checks
python host/p3a-lab/lab.py up              # test build, flash, wait for the console
python -m pytest tests/device              # flash the test build, run the device tests
cmake -S tests/host -B build-host -G Ninja; cmake --build build-host; ctest --test-dir build-host --output-on-failure
python tools/check_tree.py                 # layer rule and forbidden patterns

Set-Location <repo root>                   # raw idf.py: in the same command, every time
idf.py build
idf.py -p COM5 flash
idf.py fullclean
```

- Build output goes to `build/`. Overlay builds (test, trace, diag) go to
  `build-<name>/` and never touch the default configuration.
- `idf.py` refuses to run under an MSYS environment (Git Bash) and still
  exits 0. Run it from PowerShell, or through the lab, which strips the
  `MSYSTEM` variables.
- Before a checkpoint: the tree checks, the host tests, both builds, and the
  device tests. CI repeats all but the last on every push to `v2`.
- `idf.py monitor` and .NET `SerialPort` reset the board when they open the
  port. To read the log without a reset, open the port with DTR and RTS held
  low (pyserial: set `dtr = False` and `rts = False` before `open()`); a short
  RTS pulse then resets the board on purpose.

## Configuration and versioning

- One version number, `PROJECT_VER` at the top of the root `CMakeLists.txt`.
  It is `2.0.0-dev` until the release commit. There is no separate web UI or
  API version in 2.0.
- Kconfig is for the shape of the build only. User-changeable settings will
  live in the settings schema (M1.4); compile-time constants in one limits
  header (M1.1). The written rule lands with M1.4.

## Documentation map

| Where | What |
|-------|------|
| `docs/2.0/` | The 2.0 work stream: `README.md` (session protocol and rules), `PROGRESS.md` (the ledger), `PLAN.md` (milestones), `DECISIONS.md`, `TESTING.md`, `PARITY.md`. |
| `docs/2.0-architecture-review/` | The design: `README.md` holds the thesis and the 25-row verdict table; one page per area; `target-architecture.md` is the 2.0 skeleton; `ideas.md` is the running list of 2.0 ideas, append new ones there. |
| `docs/known-issues.md` | Open 1.x problems with root causes. Check it for every file that leaves `legacy/`. |
| `docs/deferred/` | Parked ideas, one page each. |
| `docs/art-institutions/finalized-design.md` | Source of truth for the museum channels. |
| `docs/jitter/` | The playback-jitter work: the fixes that must survive the move, the pass bar, the lab tooling. |
| `docs/reference/` | Makapix MQTT protocol and player API, ESP-IDF 5.5.4 setup. |
| `docs/infrastructure/`, `docs/HOW-TO-USE.md`, `docs/QUICK-START.md`, `README.md` | 1.x as shipped. Useful for understanding `legacy/`; rewritten at M5.4. |

Outreach, talks, and retired research corpora live in the private
`fabkury/p3a-private` repository, not here.

### Keeping documentation honest

- **When you fix a stale fact, sweep for it.** A wrong claim rarely lives in
  one place. Fix every copy together, with a commit message that states the
  current behavior.
- The ledger is updated in the same commit as the work it describes.
- Agent guidance belongs in this file. `CLAUDE.md` stays a thin import plus
  Claude-specific notes.

## Releases

The user cuts releases. 2.x has no release process yet; it is defined at
M5.3. Two facts are already fixed: 2.x firmware ships under a new asset name
so that 1.x units never see it (`docs/2.0/DECISIONS.md` D12), and nothing
installs on a device without its owner's approval. The 1.x release process is
in `AGENTS.md` on `main`.
