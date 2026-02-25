# Configuration and Development

## Kconfig Structure

Configuration is organized into component-specific Kconfig files under a unified "Physical Player of Pixel Art (P3A)" menu:

| Kconfig File | Scope |
|-------------|-------|
| `main/Kconfig.projbuild` | General, Display, Animation, Touch, Task Priorities, Wi-Fi, USB |
| `components/p3a_board_ep44b/Kconfig` | Pixel format (RGB888/RGB565), board-specific options |
| `components/pico8/Kconfig` | PICO-8 streaming options |
| `components/wifi_manager/Kconfig` | WiFi manager enable/disable |
| `components/channel_manager/Kconfig` | Channel/playlist options |
| `components/animation_decoder/Kconfig` | Decoder options |
| `components/giphy/Kconfig` | Giphy API key, rendition, format defaults |
| `components/ota_manager/Kconfig` | OTA update options |
| `components/play_scheduler/Kconfig` | Play scheduler options |
| `components/makapix/Kconfig` | Makapix Club options |

## Key Options

| Config | Default | Location | Description |
|--------|---------|----------|-------------|
| `P3A_AUTO_SWAP_INTERVAL_SECONDS` | 30 | main | Auto-swap timer (seconds) |
| `P3A_MAX_SPEED_PLAYBACK` | y | main | Ignore frame delays, play at max speed |
| `P3A_USB_MSC_ENABLE` | y | main | Enable USB Mass Storage |
| `P3A_PPA_UPSCALE_ENABLE` | y | main | Enable PPA hardware upscaling for Giphy |
| `P3A_PIXEL_FORMAT_RGB888` | y | p3a_board_ep44b | 24-bit color mode |
| `P3A_PICO8_ENABLE` | y | pico8 | Enable PICO-8 streaming |
| `P3A_PICO8_USB_STREAM_ENABLE` | y | pico8 | Enable PICO-8 USB streaming |
| `WIFI_MANAGER_ENABLED` | y | wifi_manager | Enable WiFi manager |
| `CHANNEL_MANAGER_PAGE_SIZE` | 32 | channel_manager | Channel page size (8-64) |
| `ANIMATION_DECODER_STATIC_FRAME_DELAY_MS` | 100 | animation_decoder | Static image display duration |
| `GIPHY_RENDITION_DEFAULT` | `fixed_height` | giphy | Default Giphy rendition |
| `GIPHY_FORMAT_DEFAULT` | `gif` | giphy | Default Giphy format |

---

## Storage Layout

### SD Card

All p3a data is stored under a configurable root folder (`/sdcard/p3a` by default, user-configurable via web UI):

```
/sdcard/p3a/
├── animations/    # Local animation files (WebP, GIF, PNG, JPEG)
├── vault/         # Cached artwork from Makapix (sharded by SHA256: ab/cd/<hash>.<ext>)
├── giphy/         # Cached Giphy GIFs (sharded by SHA256)
├── channel/       # Channel settings and index files
├── playlists/     # Playlist cache files
└── downloads/     # Temporary upload storage
```

- **Root folder**: Configurable via web UI (e.g., `/p3a`, `/data`). The `/sdcard` prefix is prepended automatically at runtime. Changes require reboot.
- **Supported formats**: `.webp`, `.gif`, `.png`, `.jpg`/`.jpeg`

### Vault Storage

- **Layout**: `/sdcard/p3a/vault/ab/cd/<sha256>.<ext>` (SHA256-sharded)
- **Sidecar**: `.json` metadata files alongside assets
- **Atomic writes**: `.tmp` + `fsync` + `rename`

### NVS (Non-Volatile Storage)

- **Partition**: `nvs` (24 KB)
- **Stores**: Wi-Fi credentials, device settings, playlist state, Makapix credentials, playset data

### LittleFS (Internal Flash)

- **Partition**: `storage` (4 MB)
- **Mount point**: `/webui`
- **Contents**: Web UI assets (HTML, CSS, JS), version metadata

---

## Development Workflow

### Prerequisites

1. **ESP-IDF**: v5.5.x
2. **Python**: 3.8+
3. **Hardware**: Waveshare ESP32-P4-WIFI6-Touch-LCD-4B + microSD card

### Setup

```bash
# Clone repository
git clone https://github.com/fabkury/p3a.git
cd p3a

# Activate ESP-IDF environment
. $HOME/esp/esp-idf/export.sh  # Linux/macOS
# or
C:\Users\<user>\esp\v5.5.1\esp-idf\export.ps1  # Windows PowerShell

# Set target
idf.py set-target esp32p4

# Build
idf.py build

# Flash and monitor
idf.py flash monitor
```

On Windows, set `$env:PYTHONUTF8="1"` before building to prevent Unicode encoding errors.

### Adding a New Component

1. Create directory under `components/`
2. Add `CMakeLists.txt` with `idf_component_register()`
3. Add `Kconfig` if configuration needed
4. Add component name to `REQUIRES` in `main/CMakeLists.txt`

### Version Management

The project uses three independent version numbers (all in root `CMakeLists.txt`):

| Variable | Format | Purpose |
|----------|--------|---------|
| `PROJECT_VER` | `MAJOR.MINOR.PATCH[-suffix]` | Firmware version, used by OTA |
| `WEBUI_VERSION` | `X.Y` | Web UI version, updated independently |
| `P3A_API_VERSION` | integer | HTTP API version, bumped on breaking changes |

### Testing

Manual testing workflow:
1. Build verification: `idf.py build` succeeds
2. Flash verification: `idf.py flash` succeeds
3. Functional tests: Display, touch, Wi-Fi, USB, OTA, playsets
