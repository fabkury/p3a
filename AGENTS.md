# AGENTS.md

Guidance for AI coding agents working in this repository. This is the canonical
file: `CLAUDE.md` imports it, so project guidance goes here and only
Claude-specific notes go there. It is read at the start of every session, so
keep it accurate and keep it short.

## Project overview

p3a is an ESP32-P4 Wi-Fi pixel art player. It plays WebP, GIF, PNG, APNG, JPEG,
and BMP files, animated and static: GIFs and stickers from Giphy and Klipy,
animated artworks from Makapix Club (a pixel art social network), static museum
artworks over IIIF and similar image CDNs, artworks fetched from a URL, and
local files on the SD card.

Target hardware is the Waveshare ESP32-P4-WIFI6-Touch-LCD-4B: 720x720 IPS
display (24-bit, MIPI-DSI), GT911 capacitive touch, 32 MB PSRAM, 32 MB flash,
and an ESP32-C6 Wi-Fi 6 co-processor reached through esp_hosted over SDIO.

The firmware is public (Apache 2.0) and runs on real users' devices, which
update themselves over the air from GitHub Releases. Treat `main` as
production.

## Ground rules

- **Never build, flash, or monitor on your own initiative.** Do not run
  `idf.py build`, `idf.py flash`, `idf.py monitor`, or anything else that
  compiles or flashes the firmware unless the user directly and explicitly asks
  for it, not even to "check that it compiles". The user does the building and
  the testing on hardware. A work-stream README may relax this for that stream
  (`docs/jitter/README.md` did); such a grant applies only while the user has
  you working in that stream.
- **A change is unverified until the user confirms it on the device.** Say so
  plainly. Do not describe untested work as done, working, or verified.
- **Clarifying questions are always welcome.** Ask when different readings of a
  request would lead to materially different work.

## Toolchain and environment

- ESP-IDF v5.5.4, target `esp32p4`, developed on Windows with PowerShell.
  Setting up a new machine: `docs/reference/esp-idf-5.5.4-workstation-2-crib-sheet.md`.
- The ESP32-C6 image `network_adapter.bin` is a prebuilt binary in
  `components/slave_ota/firmware/`; `slave_ota` flashes it into the P4's
  `slave_fw` partition. It is not built by this project.
- Activate the IDF environment once per shell session and reuse that session.
  Re-sourcing the profile for every command leaves stray shells behind.

  ```powershell
  $env:PYTHONUTF8 = "1"          # IDF's Python tooling raises Unicode errors on Windows without it
  . <IDF tools root>\Microsoft.v5.5.4.PowerShell_profile.ps1   # EIM install; commonly C:\Espressif\tools\
  $env:ESP_IDF_VERSION = "5.5"   # REQUIRED after activation, see next bullet
  ```

- **`ESP_IDF_VERSION` must be `5.5`, not `5.5.4`.** EIM 0.13.x's profile
  exports the full version, but official IDF 5.5.x activation exports
  major.minor, and `esp_wifi_remote`'s Kconfig keys its version fragments on
  that convention. With the wrong value the fragment silently loads nothing,
  `sdkconfig` regenerates with esp_hosted on SPI and an "invalid" slave target,
  and the build dies with `#error "Unknown Slave Target"` deep inside
  esp_hosted. Recovery: `git checkout -- sdkconfig dependencies.lock`, delete
  `build/`, set the variable, rebuild.
- **Silicon revision guard.** The board's ESP32-P4 is silicon rev v1.0.
  `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` and `CONFIG_ESP32P4_REV_MIN_1=y` must
  survive every `sdkconfig` regeneration: from IDF 5.5.2 the default is rev
  3.01 only, and the bootloader rejects such images on this board. If a
  regeneration diff touches `ESP32P4_REV_MIN_FULL`, stop and restore it.
- **Run `idf.py` from the repository root**, with the directory change in the
  same command. `idf.py` treats the current directory as the project; run from
  inside a component it configures that component as a standalone project and
  leaves a stray `build/` directory there.
- Serial ports differ by machine and by unit. Ask rather than assume. The
  jitter lab's `host/jitter-lab/find_port.ps1` probes for the board, and its
  `serial_logger.py` captures logs without resetting it.

## Build commands

Reference only; the ground rules above govern whether you may run them.

```powershell
Set-Location <repo root>               # in the same command as idf.py, every time
idf.py set-target esp32p4              # first time only
idf.py build
idf.py flash monitor                   # slave_ota appends --force to flash_args so esptool v5 accepts network_adapter.bin
idf.py menuconfig
idf.py fullclean
idf.py build -DP3A_BUILD_FLASHER=ON    # release build: also produces p3a-flasher.exe (Windows host only)
```

- Build output goes to `build/`. Every successful build also populates
  `release/v{PROJECT_VER}/` (git-ignored) with the binaries, `.sha256`
  checksums, `flash_args`, `flash_command.txt`, `manifest.json`, and a README.
  Building while `PROJECT_VER` still equals a shipped release overwrites that
  release's local folder.
- `P3A_BUILD_FLASHER` persists in the CMake cache. After a release build, pass
  `-DP3A_BUILD_FLASHER=OFF` once (or run `fullclean`) to get fast dev builds
  back.
- `sdkconfig.diag.defaults` and `sdkconfig.trace.defaults` are alternate
  configurations for diagnostic builds from the jitter work stream. Do not fold
  their options into the release `sdkconfig`. See `docs/jitter/README.md`.

## Architecture

### Entry point and core flow

- `main/p3a_main.c`: boot sequence. NVS init, LittleFS mount, LCD/touch/USB/Wi-Fi
  init, HTTP server, Makapix connection.
- `main/display_renderer.c`: frame buffer management with triple buffering and
  VSYNC.
- `main/animation_player.c`, `animation_player_render.c`,
  `animation_player_loader.c`: animation decode/render pipeline.
- `main/playback_controller.c`: switches between animation, PICO-8, and UI
  render sources.

System diagram, boot sequence, and the service layer (`main/*_service.c`) are
described in `docs/infrastructure/architecture.md`.

### Key components (in `components/`)

| Component | Purpose |
|-----------|---------|
| `p3a_core` | Unified state machine and lifecycle management |
| `play_scheduler` | Playback engine that executes playsets to select artwork |
| `channel_manager` | Playlist/channel handling, vault storage (hash-sharded `/sdcard/p3a/vault/`) |
| `giphy` | Giphy API integration: trending GIFs, on-demand download, SD card caching (`/sdcard/p3a/giphy/`) |
| `klipy` | Klipy API integration: GIF/sticker trending, search, and category channels; BYOK API key (partner.klipy.com), on-demand download, SD card caching (`/sdcard/p3a/klipy/{gif\|sticker}/`) |
| `art_institution` | Museum channels via IIIF (plus Cleveland's and Minneapolis's non-IIIF fixed-rendition CDNs). Nine museums today (`artic`, `rijks`, `vam`, `wellcome`, `smk`, `ham`, `si`, `cma`, `mia`); per-museum dispatch (refresh, image URL build, optional resolver such as the Rijks Linked-Art walk), shared rate-limit cooldown synchronized with the browser via `/api/museum/rate-limits*`, vault at `/sdcard/p3a/museum/{museum_id}/`. NVS settings: `ai_refresh_sec`, `ai_cache_size`, plus per-museum BYOK keys where required (`ham_api_key`, `si_api_key`). AIC is currently marked unavailable (`unavailable_reason` in the dispatch table): its image host blocks non-browser clients via Cloudflare since 2026-08; cached AIC art still plays. See `docs/art-institutions/finalized-design.md`. |
| `animation_decoder` | WebP/PNG/APNG/JPEG/BMP decoders with transparency support. APNG decode relies on `components/espressif__libpng`, a **local fork** of the managed libpng 1.6.52 component carrying the official APNG patch (read-only). On any libpng bump the matching patch must be re-applied; see that component's README.md |
| `animated_gif_decoder` | GIF decoder (C++ wrapper) |
| `wifi_manager` | Wi-Fi provisioning, captive portal, mDNS (`p3a.local`) |
| `http_api` | REST API and WebSocket server |
| `config_store` | NVS-backed persistent configuration |
| `makapix` | MQTT over TLS for Makapix Club integration |
| `ota_manager` | Wireless firmware updates from GitHub Releases |
| `slave_ota` | ESP32-C6 co-processor auto-flash |
| `p3a_board_ep44b` | Hardware abstraction layer |

Supporting components:

| Component | Purpose |
|-----------|---------|
| `content_cache` | Download state and channel cache management |
| `loader_service` | Animation file loader |
| `playback_queue` | Current/next/prev/peek swap requests |
| `pin_lists` | Pinned-artwork vault: multiple named lists, each a first-class channel, under `/sdcard/p3a/pinned/` |
| `show_url` | Download an artwork from a URL and play it |
| `storage_eviction` | Age-based cleanup of cache debris (artwork, `.404` markers, orphaned `.tmp` files) and emptied directories |
| `http_fetch` | Shared HTTP fetch/download helper and TLS concurrency gate. Every HTTPS request goes through it except the firmware image download (`esp_https_ota`) and the MQTT link |
| `event_bus` | Typed events with categories, subscribe/emit |
| `sdio_bus` | Coordinates the SDMMC controller shared between Wi-Fi (SDIO slot 1 via esp_hosted) and the SD card (slot 0) |
| `sd_idle_wait` | Yielding replacement for IDF's `sdmmc_wait_for_idle()` (jitter fix 8) |
| `pico8` | PICO-8 game streaming from a browser-hosted emulator over WebSocket, including audio to the on-board speaker |
| `ugfx` | uGFX, on-screen text and font rendering |
| `libwebp_decoder` | Exposes upstream libwebp to ESP-IDF |
| `mem_stats` | Shared memory-usage snapshot helpers |
| `frame_trace` | Presentation-lateness trace; compiles to nothing unless `CONFIG_P3A_FRAME_TRACE` is set |
| `debug_http_log` | Performance instrumentation behind `CONFIG_P3A_PERF_DEBUG`; no-ops when off |

Per-component detail: `docs/infrastructure/components.md` and each component's
header overview comment.

### Storage layout

- **LittleFS** `/webui` (4 MB): web UI assets.
- **SD card** `/sdcard`: all artwork storage. `/sdcard/p3a` is the default root
  only; the real root is a runtime setting, and code resolves paths through
  `sd_path` instead of hard-coding it.
  - `/sdcard/p3a/animations/`: local files
  - `/sdcard/p3a/vault/`: cached Makapix artwork
  - `/sdcard/p3a/pinned/`: pinned-artwork lists
  - `/sdcard/p3a/giphy/`: cached Giphy artwork
  - `/sdcard/p3a/klipy/`: cached Klipy artwork
  - `/sdcard/p3a/museum/{museum_id}/`: cached art-institution artwork
- **NVS** (64 KB): Wi-Fi credentials, settings, state.

Partition sizes and offsets are in `partitions.csv`.

## Configuration and versioning

Three independent version numbers live at the top of the root `CMakeLists.txt`:

| Variable | Format | Meaning |
|----------|--------|---------|
| `PROJECT_VER` | `MAJOR.MINOR.PATCH[-suffix]` | Firmware version, embedded in the binary and compared by OTA |
| `WEBUI_VERSION` | `X.Y` | Web UI version, updated independently of the firmware |
| `P3A_API_VERSION` | integer | HTTP API version. Bump only for breaking endpoint changes; new firmware must keep working with older web UIs at the same value |

- Bump the web UI by editing `WEBUI_VERSION` only. CMake regenerates
  `webui/metadata.json`, `webui/version.txt`, and `webui/static/compat.js`
  (from `compat.js.in`) at configure time. Never hand-edit those three, even
  though `metadata.json` and `compat.js` are tracked in git; they are build
  outputs and the next configure overwrites them.
- Devices decide whether to install a new web UI by comparing versions, so a
  `webui/` change that ships without a `WEBUI_VERSION` bump never reaches
  devices already on that version.
- Kconfig: `main/Kconfig.projbuild` holds the main options
  (`P3A_AUTO_SWAP_INTERVAL_SECONDS`, `P3A_USB_MSC_ENABLE`, task priorities, PPA
  upscaling, animations directory). Component-specific options live in each
  component's `Kconfig`, for example `P3A_PICO8_ENABLE` in `components/pico8`
  and `OTA_FIRMWARE_ASSET_NAME` in `components/ota_manager`.
- Runtime settings (Wi-Fi credentials, API keys, refresh intervals, museum
  settings such as `ai_refresh_sec` and `ai_cache_size`) live in NVS through
  `config_store`, not in Kconfig. Overview of both:
  `docs/infrastructure/configuration-and-development.md`.

## Documentation map

Most non-trivial decisions in this project are written down. Read the relevant
page before exploring the code for a subsystem, and check the parked
investigations before re-opening a problem.

| Where | What |
|-------|------|
| `docs/infrastructure/README.md` | Developer index: architecture, directory structure, build system, components, hardware, network and API, display pipeline, configuration. `docs/INFRASTRUCTURE.md` is a stub that points here. |
| `docs/QUICK-START.md`, `docs/HOW-TO-USE.md` | User-facing behavior, from first boot to the REST API. The source of truth for what the product does from the user's side. |
| `docs/flash-p3a.md`, `docs/web-flasher/` | Flashing methods, and the browser flasher served from GitHub Pages (`firmware/{tag}/` is filled by CI on each release). |
| `docs/BOARD-CAPABILITIES.md` | Hardware reference for the board. |
| `docs/reference/` | Makapix MQTT protocol and player API, ESP-IDF workstation crib sheet. |
| `docs/art-institutions/finalized-design.md` | Source of truth for the museum channels; per-museum quirks and status live there. |
| `docs/jitter/`, `docs/intro-animations/`, `docs/klipy/`, `docs/makapix-cert-renewal/`, `docs/transport-recovery/`, `docs/title-view/` | Per-work-stream folders. Where a `README.md` exists it is the "start here" for resuming that stream; otherwise start from `PLAN.md`. |
| `docs/*-tabled.md`, `docs/deferred/`, and the standalone evaluations in `docs/` | Investigations parked with their findings (concurrent TLS EAGAIN, CPU1 saturation, SDIO RX OOM, exFAT, PSRAM migration, content-source survey). |
| `docs/outreach/`, `docs/brand-identity/` | Public communication drafts and brand work, not firmware. |
| `README.md` | Public front page. Its feature list, component table, and storage layout must match reality. |

### Keeping documentation honest

- **When you fix a stale fact, sweep for it.** A wrong claim rarely lives in
  one place. Check `docs/`, `README.md`, header overview comments, web UI copy
  in `webui/`, on-device hint strings, and dead declarations that exist only
  because of the old behavior. Fix them all together, with a commit message
  that states the current behavior.
- Behavior changes carry their documentation with them: update the affected
  `docs/` page and `README.md` section, and bump `WEBUI_VERSION` when web UI
  copy changes.
- Agent guidance belongs in this file. `CLAUDE.md` stays a thin import plus
  Claude-specific notes.

## Releases

The user cuts releases. This section exists so that preparation work (version
bumps, notes, asset checks) is done correctly.

- Releases are GitHub Releases on `fabkury/p3a`, tagged `v{PROJECT_VER}` on the
  commit that bumps `PROJECT_VER`. `PROJECT_VER` stays at the released value
  afterwards; there is no `-dev` suffix between releases.
- **The asset set is an OTA contract.** Devices poll the latest releases for
  `p3a.bin` and `p3a.bin.sha256` (asset name from `OTA_FIRMWARE_ASSET_NAME`);
  web UI OTA reads `manifest.json` and `storage.bin` from the same release.
  Renaming or omitting any of these breaks updates for every deployed unit.
  Ship everything in `release/v{PROJECT_VER}/`, built with
  `-DP3A_BUILD_FLASHER=ON` so `p3a-flasher.exe` is included.
- Publish with `gh release create v{PROJECT_VER} --notes-file <notes> release/v{PROJECT_VER}/*`.
  The `publish-firmware-to-pages` workflow then copies the flash assets into
  `docs/web-flasher/firmware/{tag}/` on `main` for the browser flasher.
- Release notes are short: bullets only, headline features only, no internal
  fixes, no changelog link, no em dashes.
- A release is done when the user has verified the field OTA from the previous
  release on a real device.
