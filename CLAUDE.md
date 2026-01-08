# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

p3a is an ESP32-P4 Wi-Fi pixel art player that displays animated WebP/GIF/PNG/JPEG files. It connects to Makapix Club (a pixel art social network) and runs on the Waveshare ESP32-P4-WIFI6-Touch-LCD-4B board (720×720 IPS + GT911 touch + ESP32-C6 Wi-Fi 6 co-processor).

## Build Commands

```bash
# Activate ESP-IDF (Windows PowerShell)
C:\Users\Fab\esp\v5.5.1\esp-idf\export.ps1

# IMPORTANT: Set PYTHONUTF8 to avoid Unicode encoding errors on Windows
$env:PYTHONUTF8="1"

# Set target (first time only)
idf.py set-target esp32p4

# Build
idf.py build

# Flash and monitor
idf.py flash monitor

# Configure options
idf.py menuconfig

# Clean build
idf.py fullclean
```

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
| `channel_manager` | Playlist/channel handling, vault storage (SHA256-sharded `/sdcard/p3a/vault/`) |
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
- **LittleFS** `/spiffs` (4MB) - Web UI assets
- **SD Card** `/sdcard` - Artwork storage
  - `/sdcard/p3a/animations/` - Local files
  - `/sdcard/p3a/vault/` - Cached Makapix artwork
- **NVS** (24KB) - Wi-Fi credentials, settings, state

### Flash Partitions (`partitions.csv`)
Dual OTA slots (8MB each), NVS, LittleFS (4MB), and a 2MB partition for ESP32-C6 firmware.

## Configuration

- **Version**: Set in root `CMakeLists.txt` line ~14 (e.g., `set(VERSION "0.6.5-dev")`)
- **Kconfig**: `main/Kconfig.projbuild` for main options, component-specific Kconfig in each component
- Key options: `P3A_AUTO_SWAP_INTERVAL_SECONDS`, `P3A_PICO8_ENABLE`, `P3A_USB_MSC_ENABLE`

## HTTP API (at `http://p3a.local/`)

- `GET /status` - Device status JSON
- `POST /action/swap_next`, `/action/swap_back` - Navigation
- `GET /config`, `POST /config` - Settings
- `GET /files/list`, `POST /files/upload` - File management
- `WebSocket /ws/pico8` - PICO-8 binary stream

## Adding a New Component

1. Create directory under `components/`
2. Add `CMakeLists.txt` with `idf_component_register()`
3. Add `Kconfig` if configuration needed
4. Add component name to `REQUIRES` in `main/CMakeLists.txt`

## Documentation

- `docs/INFRASTRUCTURE.md` - Comprehensive technical architecture
- `docs/HOW-TO-USE.md` - User guide
- `docs/flash-p3a.md` - Flashing instructions
- `docs/ROADMAP.md` - Development plan
