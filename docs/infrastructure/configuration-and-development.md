# Configuration and Development

## Kconfig Structure

Configuration is organized into a project-level Kconfig (`main/Kconfig.projbuild`, which appears under the "Physical Player of Pixel Art (P3A)" menu) plus several component-specific Kconfig menus. They appear as siblings in `idf.py menuconfig`, not under a single unified menu.

| Kconfig File | Scope |
|-------------|-------|
| `main/Kconfig.projbuild` | General, Display, Animation, Touch, Task Priorities, Wi-Fi, USB |
| `components/p3a_board_ep44b/Kconfig` | Pixel format (RGB888/RGB565), board-specific options |
| `components/pico8/Kconfig` | PICO-8 streaming options |
| `components/wifi_manager/Kconfig` | WiFi manager enable/disable, SNTP server |
| `components/channel_manager/Kconfig` | Channel/playlist options |
| `components/animation_decoder/Kconfig` | Decoder options |
| `components/giphy/Kconfig` | Giphy API key, rendition, format defaults |
| `components/ota_manager/Kconfig` | OTA update options |
| `components/play_scheduler/Kconfig` | Play scheduler options |
| `components/makapix/Kconfig` | Makapix Club options |
| `components/storage_eviction/Kconfig` | Storage eviction / cache cleanup options |

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

### Museum (NVS, runtime — not Kconfig)

The two museum-channel knobs are stored in NVS and edited from the **Museum** tab of the settings page (`http://p3a.local/settings#museum`). They are not compile-time Kconfig options.

| NVS key | Default | Allowed values | Description |
|---------|---------|----------------|-------------|
| `ai_refresh_sec` | `86400` (1 day) | `28800`, `86400`, `172800`, `345600` (8 h / 1 d / 2 d / 4 d) | How often the device re-queries each saved museum channel's listing API |
| `ai_cache_size` | `1024` | `32`, `64`, `128`, `256`, `512`, `1024`, `2048`, `4096` | Maximum cache entries per museum channel (FIFO-trimmed by insertion order; subject to the absolute `CHANNEL_CACHE_HARD_CAP` of 4096) |

---

## Storage Layout

### SD Card

All p3a data is stored under a configurable root folder (`/sdcard/p3a` by default, user-configurable via web UI):

```
/sdcard/p3a/
├── animations/                 # Local animation files (WebP, GIF, PNG/APNG, JPEG)
├── vault/                      # Cached artwork from Makapix (hash-sharded: {0..63}/{0..63}/<storage_key>.<ext>)
├── giphy/                      # Cached Giphy GIFs (hash-sharded)
├── museum/                     # Cached museum (IIIF) artwork, partitioned per museum
│   ├── artic/                  #   Art Institute of Chicago (hash-sharded: {0..63}/{0..63}/<iiif_key>.<ext>)
│   ├── rijks/                  #   Rijksmuseum
│   ├── vam/                    #   Victoria and Albert Museum
│   ├── wellcome/               #   Wellcome Collection
│   └── smk/                    #   Statens Museum for Kunst
├── channel/                    # Per-channel JSON metadata files
├── playlists/                  # Playlist cache files
└── temporary/                  # Staging area for uploads and downloads
```

- **Root folder**: Configurable via web UI (e.g., `/p3a`, `/data`). The `/sdcard` prefix is prepended automatically at runtime. Changes require reboot.
- **Supported formats**: `.webp`, `.gif`, `.png`/`.apng`, `.jpg`/`.jpeg`

### Vault Storage

- **Layout** (v1.0 on-disk contract): `/sdcard/p3a/vault/{d0}/{d1}/<storage_key>.<ext>` — `SD_SHARD_DEPTH` (2) shard levels named in decimal `0`–`63`, where `d_i` = bits `[8i, 8i+5]` (byte `i` masked to `SD_SHARD_MASK` = 6 bits) of the 64-bit FNV-1a hash (fmix64-finalized) of the FAT-sanitized leaf filename. The filename is the storage key UUID; because the hash input is the sanitized filename itself, a file's shard location is always re-derivable from its name. Path construction is centralized in `sd_path_build_sharded()` (`p3a_core`). Pre-1.0 firmware used a 3-level SHA256 shard (`aa/bb/cc/`); those trees were orphaned without migration — the layout-unaware age-based eviction reclaims their files and dissolves the emptied directories under space pressure, or delete them manually for immediate space.
- **Sidecar**: `.404` markers indicate prior failed downloads (placed next to where the asset would have lived). Per-channel JSON metadata files live in `/sdcard/p3a/channel/{channel_id}.json`, not in the vault.
- **Atomic writes**: `.tmp` + `fsync` + `rename`

### NVS (Non-Volatile Storage)

- **Partition**: `nvs` (64 KB)
- **Stores**: Wi-Fi credentials, device settings, playlist state, Makapix credentials, playset data

### LittleFS (Internal Flash)

- **Partition**: `storage` (4 MB)
- **Mount point**: `/webui`
- **Contents**: Web UI assets (HTML, CSS, JS), version metadata

---

## Development Workflow

### Prerequisites

1. **ESP-IDF**: v5.5.x
2. **Python**: 3.9+ (required by ESP-IDF v5.5.x)
3. **Hardware**: Waveshare ESP32-P4-WIFI6-Touch-LCD-4B + microSD card

### Setup

```bash
# Clone repository
git clone https://github.com/fabkury/p3a.git
cd p3a

# Activate ESP-IDF environment
. $HOME/esp/esp-idf/export.sh  # Linux/macOS
# or
. C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1  # Windows PowerShell
$env:ESP_IDF_VERSION="5.5"  # required: EIM sets full version, IDF Kconfig needs major.minor

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
