# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

p3a is an ESP32-P4 Wi-Fi pixel art player that displays animated WebP/GIF/PNG/JPEG files. It plays GIFs from Giphy, animated artworks from Makapix Club (a pixel art social network), static museum artworks over IIIF, and local files. Runs on the Waveshare ESP32-P4-WIFI6-Touch-LCD-4B board (720×720 IPS 24-bit + GT911 touch + ESP32-C6 Wi-Fi 6 co-processor).

## Build Management

The build commands are listed below for your information, but you must NOT build the project unless directly and explicitly requested to do so by the user. The user will be doing the building and testing.

## Build Commands

```powershell
# Set PYTHONUTF8 to avoid Unicode encoding errors on Windows
$env:PYTHONUTF8="1"

# Activate ESP-IDF v5.5.4 (Windows PowerShell): avoid running this command multiple times because you don't want to leave open sessions behind. Instead, run the activation once, and reuse the environment on subsequent commands
. C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1
# REQUIRED after activating: EIM 0.13.1's profile script sets ESP_IDF_VERSION to the full
# version ("5.5.4"), but official IDF 5.5.x activation sets major.minor ("5.5"), which is
# what esp_wifi_remote's Kconfig keys its version fragments on. Without this override the
# fragment silently fails to load, sdkconfig regenerates with esp_hosted on SPI/"invalid"
# slave target, and the build dies with "Unknown Slave Target".
$env:ESP_IDF_VERSION="5.5"

# Set target (first time only)
idf.py set-target esp32p4

# Build
idf.py build

# Flash and monitor (board is usually on COM11)
# (slave_ota's CMakeLists appends --force to flash_args: esptool v5 would otherwise
# refuse network_adapter.bin, the ESP32-C6 image flashed into the P4's slave_fw
# partition by design)
idf.py flash monitor

# Configure options
idf.py menuconfig

# Clean build
idf.py fullclean
```

**sdkconfig caution:** the board's ESP32-P4 is silicon rev v1.0. `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` + `CONFIG_ESP32P4_REV_MIN_1=y` must survive any sdkconfig regeneration — from IDF 5.5.2 the default is rev-3.01-only, whose images the bootloader rejects on this board. If a regeneration diff touches `ESP32P4_REV_MIN_FULL`, stop and restore.

Build artifacts go to `build/`. Release binaries are copied to `release/v{VERSION}/`.

## Architecture

### Entry Point and Core Flow
- `main/p3a_main.c` - Boot sequence: NVS init → LittleFS mount → LCD/touch/USB/WiFi init → HTTP server → Makapix connection
- `main/display_renderer.c` - Frame buffer management with triple buffering and VSYNC
- `main/animation_player.c` + `animation_player_render.c` + `animation_player_loader.c` - Animation decode/render pipeline
- `main/playback_controller.c` - Switches between animation, PICO-8, and UI render sources

### Key Components (in `components/`)
| Component | Purpose |
|-----------|---------|
| `p3a_core` | Unified state machine and lifecycle management |
| `play_scheduler` | Playback engine that executes playsets to select artwork |
| `channel_manager` | Playlist/channel handling, vault storage (hash-sharded `/sdcard/p3a/vault/`) |
| `giphy` | Giphy API integration: trending GIFs, on-demand download, SD card caching (`/sdcard/p3a/giphy/`) |
| `art_institution` | Museum channels via IIIF. Seven museums today (`artic`, `rijks`, `vam`, `wellcome`, `smk`, `ham`, `si`); per-museum dispatch (refresh, IIIF URL build, optional resolver e.g. Rijks Linked-Art walk), shared rate-limit cooldown synchronized with the browser via `/api/museum/rate-limits*`, vault at `/sdcard/p3a/museum/{museum_id}/`. NVS settings: `ai_refresh_sec`, `ai_cache_size`, plus per-museum BYOK keys where required (`ham_api_key`, `si_api_key`). See `docs/art-institutions/finalized-design.md`. |
| `animation_decoder` | WebP/PNG/JPEG decoders with transparency support |
| `animated_gif_decoder` | GIF decoder (C++ wrapper) |
| `wifi_manager` | Wi-Fi provisioning, captive portal, mDNS (`p3a.local`) |
| `http_api` | REST API and WebSocket server |
| `config_store` | NVS-backed persistent configuration |
| `makapix` | MQTT over TLS for Makapix Club integration |
| `ota_manager` | Wireless firmware updates from GitHub Releases |
| `slave_ota` | ESP32-C6 co-processor auto-flash |
| `p3a_board_ep44b` | Hardware abstraction layer |

### Storage Layout
- **LittleFS** `/webui` (4MB) - Web UI assets
- **SD Card** `/sdcard` - Artwork storage
  - `/sdcard/p3a/animations/` - Local files
  - `/sdcard/p3a/vault/` - Cached Makapix artwork
  - `/sdcard/p3a/giphy/` - Cached Giphy artwork
  - `/sdcard/p3a/museum/{museum_id}/` - Cached art-institution artwork
- **NVS** (64KB) - Wi-Fi credentials, settings, state

### Flash Partitions (`partitions.csv`)
Dual OTA slots (8MB each), NVS, LittleFS (4MB), and a 2MB partition for ESP32-C6 firmware.

## Configuration

- **Version**: Set in root `CMakeLists.txt` line ~14 (e.g., `set(VERSION "0.6.5-dev")`)
- **Kconfig**: `main/Kconfig.projbuild` for main options, component-specific Kconfig in each component
- Key options: `P3A_AUTO_SWAP_INTERVAL_SECONDS`, `P3A_PICO8_ENABLE`, `P3A_USB_MSC_ENABLE`

## Adding a New Component

1. Create directory under `components/`
2. Add `CMakeLists.txt` with `idf_component_register()`
3. Add `Kconfig` if configuration needed
4. Add component name to `REQUIRES` in `main/CMakeLists.txt`

## Documentation

- `docs/INFRASTRUCTURE.md` - Comprehensive technical architecture
- `docs/HOW-TO-USE.md` - User guide

## Additional comments

Clarifying questions are always welcome.
