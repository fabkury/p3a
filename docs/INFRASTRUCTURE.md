# p3a Infrastructure Documentation

This document provides a comprehensive overview of the p3a (Pixel Pea) firmware infrastructure, architecture, and development environment.

## Table of Contents

1. [Project Overview](#project-overview)
2. [Architecture Overview](#architecture-overview)
3. [Directory Structure](#directory-structure)
4. [Build System](#build-system)
5. [Component Architecture](#component-architecture)
6. [Hardware Interfaces](#hardware-interfaces)
7. [Network Stack](#network-stack)
8. [Storage and Filesystem](#storage-and-filesystem)
9. [Display Pipeline](#display-pipeline)
10. [Touch Input System](#touch-input-system)
11. [USB Functionality](#usb-functionality)
12. [Web UI and REST API](#web-ui-and-rest-api)
13. [Configuration System](#configuration-system)
14. [Development Workflow](#development-workflow)

---

## Project Overview

**p3a (Pixel Pea)** is an ESP32-P4-powered Wi-Fi art frame that displays pixel art animations. It integrates:

- **Microcontroller**: Dual-core ESP32-P4 with ESP32-C6 for Wi-Fi 6/BLE
- **Display**: 720×720 pixel IPS touchscreen with 24-bit color
- **Storage**: microSD card via SDMMC interface
- **Connectivity**: Wi-Fi, HTTP server, WebSocket, REST API, mDNS, MQTT
- **I/O**: Capacitive touch, USB-C (CDC-ACM, Mass Storage, vendor bulk pipe)
- **Framework**: ESP-IDF v5.5.x

### Key Features

- Animated WebP, GIF, PNG, and JPEG playback
- **Transparency support** for WebP, GIF, and PNG with configurable background color
- **Aspect ratio preservation** for non-square artworks
- Touch gestures for navigation and brightness control
- Web-based control panel at `http://p3a.local/`
- **Makapix Club integration** — send artworks directly from [dev.makapix.club](https://dev.makapix.club/)
- **Over-the-Air updates** — install firmware updates wirelessly via web UI
- **ESP32-C6 auto-flash** — co-processor firmware is updated automatically when needed
- PICO-8 game monitor mode with WebSocket streaming
- USB Mass Storage for SD card access
- Captive portal for Wi-Fi provisioning

### Codebase Statistics

- **Custom components**: 12+ ESP-IDF components
- **Main application files**: ~11 C source files
- **Build artifacts**: ~22MB (compiled binaries + SPIFFS image)

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                         p3a_main.c                          │
│                    (Application Entry Point)                │
└──────────────────────┬──────────────────────────────────────┘
                       │
        ┌──────────────┼──────────────┬─────────────┐
        │              │              │             │
   ┌────▼─────┐  ┌────▼────┐   ┌────▼────┐   ┌───▼────┐
   │ app_lcd  │  │app_touch│   │wifi_mgr │   │app_usb │
   │ (Display)│  │ (Touch) │   │(Wi-Fi)  │   │ (USB)  │
   └────┬─────┘  └────┬────┘   └────┬────┘   └───┬────┘
        │             │              │            │
   ┌────▼─────────────▼──────────────▼────────────▼────┐
   │              display_renderer.c                    │
   │           (Frame Buffer Management)                │
   └────┬──────────────────────────────────────────────┘
        │
   ┌────▼──────────────────────────────────────────────┐
   │             animation_player.c                     │
   │          (Animation Playback Engine)               │
   └────┬───────────────────┬──────────────────────────┘
        │                   │
   ┌────▼─────┐      ┌─────▼──────────────────────┐
   │ Decoders │      │   Components               │
   │ animation│      │ - channel_manager          │
   │ _decoder │      │ - p3a_board_ep44b         │
   └──────────┘      │ - wifi_manager            │
                     │ - pico8                    │
                     │ - config_store            │
                     │ - http_api                │
                     │ - makapix                 │
                     └────────────────────────────┘
```

### Execution Flow

1. **Initialization** (`app_main`):
   - NVS flash initialization
   - Network interface setup
   - Board SPIFFS filesystem mount (`p3a_board_spiffs_mount`)
   - LCD, touch, USB, and Wi-Fi initialization
   - HTTP server startup
   - Makapix Club connection (if provisioned)

2. **Runtime Operation**:
   - Display renderer manages frame buffers and vsync
   - Animation player task renders frames to LCD
   - Touch task processes gestures
   - Auto-swap task cycles animations on timer
   - HTTP server handles REST API and WebSocket connections
   - USB tasks handle CDC-ACM and Mass Storage
   - MQTT client handles cloud commands

---

## Directory Structure

```
p3a/
├── main/                      # Main application component
│   ├── p3a_main.c            # Application entry point
│   ├── app_lcd_p4.c          # LCD initialization and control
│   ├── app_touch.c           # Touch input handling
│   ├── app_usb.c             # USB composite device
│   ├── usb_descriptors.c     # USB device descriptors
│   ├── display_renderer.c    # Frame buffer management
│   ├── animation_player.c    # Core animation engine
│   ├── animation_player_render.c   # Frame rendering
│   ├── animation_player_loader.c   # Asset loading
│   ├── playback_controller.c # Playback source management
│   ├── ugfx_ui.c             # µGFX-based UI rendering
│   ├── CMakeLists.txt        # Main component build config
│   ├── Kconfig.projbuild     # Configuration menu items
│   └── include/              # Public headers
│       ├── animation_player.h
│       ├── app_lcd.h
│       ├── app_touch.h
│       ├── app_usb.h
│       ├── display_renderer.h
│       ├── playback_controller.h
│       ├── ugfx_ui.h
│       └── version.h
│
├── components/                # Custom components
│   ├── p3a_board_ep44b/      # Board abstraction (EP44B hardware)
│   │   ├── include/p3a_board.h  # Public board API
│   │   ├── p3a_board_display.c  # Display hardware
│   │   ├── p3a_board_fs.c       # SPIFFS initialization
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   │
│   ├── channel_manager/       # Channel/playlist management
│   │   ├── include/
│   │   │   ├── channel_interface.h    # Generic channel interface
│   │   │   ├── channel_player.h       # Playback state machine
│   │   │   ├── sdcard_channel.h       # SD card channel types
│   │   │   ├── sdcard_channel_impl.h  # SD card implementation
│   │   │   ├── makapix_channel_impl.h # Makapix channel stubs
│   │   │   ├── vault_storage.h        # SHA256-sharded storage
│   │   │   └── animation_metadata.h   # JSON sidecar metadata
│   │   ├── sdcard_channel.c
│   │   ├── sdcard_channel_impl.c
│   │   ├── makapix_channel_impl.c
│   │   ├── channel_player.c
│   │   ├── vault_storage.c
│   │   ├── animation_metadata.c
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   │
│   ├── wifi_manager/          # Wi-Fi and SNTP
│   │   ├── include/
│   │   │   ├── app_wifi.h
│   │   │   └── sntp_sync.h
│   │   ├── app_wifi.c         # Wi-Fi, captive portal
│   │   ├── sntp_sync.c        # NTP time sync
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   │
│   ├── pico8/                 # PICO-8 streaming support
│   │   ├── include/
│   │   │   ├── pico8_stream.h
│   │   │   ├── pico8_render.h
│   │   │   └── pico8_logo_data.h
│   │   ├── pico8_stream.c      # WebSocket streaming
│   │   ├── pico8_render.c      # 128x128 frame rendering
│   │   ├── pico8_stream_stubs.c # Stubs when disabled
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   │
│   ├── animation_decoder/     # Image/animation decoders
│   │   ├── include/
│   │   │   ├── animation_decoder.h
│   │   │   └── animation_decoder_internal.h
│   │   ├── webp_animation_decoder.c
│   │   ├── png_animation_decoder.c
│   │   ├── jpeg_animation_decoder.c
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   │
│   ├── animated_gif_decoder/  # GIF decoder (C++ wrapper)
│   │   ├── AnimatedGIF.cpp
│   │   ├── gif_animation_decoder.cpp
│   │   └── gif.inl
│   │
│   ├── app_state/             # Application state management
│   ├── config_store/          # NVS-backed configuration
│   ├── http_api/              # HTTP server and REST API
│   ├── makapix/               # Makapix Club integration (MQTT, artwork sending)
│   ├── ota_manager/           # OTA firmware updates from GitHub Releases
│   ├── slave_ota/             # ESP32-C6 co-processor auto-flash
│   ├── ugfx/                  # µGFX graphics library
│   └── libwebp_decoder/       # libwebp wrapper
│
├── webui/                     # Web interface files
│   ├── static/               # Static assets (CSS, JS, WASM)
│   └── pico8/
│       └── index.html        # PICO-8 web interface
│
├── docs/                      # Documentation
│   ├── INFRASTRUCTURE.md     # This document
│   ├── HOW-TO-USE.md
│   ├── ROADMAP.md
│   └── flash-p3a.md
│
├── build/                     # Build output directory
├── managed_components/        # Auto-downloaded ESP-IDF components
├── CMakeLists.txt            # Root CMake configuration
├── partitions.csv            # Flash partition layout
├── sdkconfig                 # ESP-IDF configuration (generated)
└── README.md                 # User-facing documentation
```

### Key Files

- **CMakeLists.txt**: Root build configuration, includes SPIFFS image creation
- **partitions.csv**: Defines flash memory layout (NVS, OTA partitions, SPIFFS)
- **sdkconfig**: ESP-IDF project configuration (auto-generated from menuconfig)
- **dependencies.lock**: Pinned versions of ESP Component Registry dependencies

---

## Build System

### Framework: ESP-IDF 5.5.x

The project uses **ESP-IDF** (Espressif IoT Development Framework) with CMake as the build system.

#### Build Configuration

```cmake
# Root CMakeLists.txt
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
add_compile_options(-Wno-ignored-qualifiers)
project(p3a)
spiffs_create_partition_image(storage webui FLASH_IN_PROJECT)
```

Key features:
- **Target**: `esp32p4`
- **Compiler flags**: `-Wno-ignored-qualifiers` to suppress qualifier warnings
- **SPIFFS**: Web UI assets compiled into a partition image

#### Main Component Registration

From `main/CMakeLists.txt`:

```cmake
set(srcs
    "app_lcd_p4.c"
    "app_touch.c"
    "p3a_main.c"
    "display_renderer.c"
    "animation_player.c"
    "animation_player_render.c"
    "animation_player_loader.c"
    "playback_controller.c"
    "ugfx_ui.c"
)

# Conditional USB files
if(CONFIG_P3A_USB_MSC_ENABLE)
    list(APPEND srcs "app_usb.c" "usb_descriptors.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include" "."
    REQUIRES driver animation_decoder p3a_board_ep44b 
             waveshare__esp32_p4_wifi6_touch_lcd_4b espressif__esp_lcd_touch 
             app_state config_store http_api json nvs_flash esp_netif 
             esp_hosted tinyusb esp_tinyusb esp_driver_sdmmc spiffs 
             makapix ugfx ota_manager slave_ota channel_manager 
             wifi_manager pico8
)
```

### Build Commands

```bash
# Set target (first time only)
idf.py set-target esp32p4

# Configure project
idf.py menuconfig

# Build project
idf.py build

# Flash and monitor
idf.py flash monitor
```

---

## Component Architecture

### Custom Components

#### 1. **p3a_board_ep44b** (Board Abstraction)
- **Purpose**: Hardware abstraction for EP44B board (ESP32-P4-WIFI6-Touch-LCD-4B)
- **Files**: `p3a_board_display.c`, `p3a_board_fs.c`
- **Public API**: `p3a_board.h`
- **Key constants**:
  - `P3A_DISPLAY_WIDTH` (720), `P3A_DISPLAY_HEIGHT` (720)
  - `P3A_PIXEL_RGB888` / `P3A_PIXEL_RGB565`
  - `P3A_HAS_TOUCH`, `P3A_HAS_WIFI`, `P3A_HAS_USB`, etc.
- **Functions**: `p3a_board_display_init()`, `p3a_board_spiffs_mount()`, `p3a_board_set_brightness()`, etc.

#### 2. **channel_manager** (Playlist/Channel Management)
- **Purpose**: Manages animation channels, playlists, and vault storage
- **Key interfaces**:
  - `channel_interface.h`: Generic channel abstraction (`load`, `next_item`, `start_playback`)
  - `sdcard_channel_impl.h`: SD card file scanning and playback
  - `makapix_channel_impl.h`: Makapix Club channel (stubs, future MQTT-based)
  - `vault_storage.h`: SHA256-sharded artwork storage
  - `animation_metadata.h`: JSON sidecar metadata parsing
- **Kconfig**: `CONFIG_CHANNEL_PLAYER_MAX_POSTS`, `CONFIG_SDCARD_CHANNEL_PAGE_SIZE`

#### 3. **wifi_manager** (Connectivity)
- **Purpose**: Wi-Fi STA/AP mode, captive portal, SNTP synchronization
- **Files**: `app_wifi.c`, `sntp_sync.c`
- **Features**:
  - Auto-connect to saved credentials
  - Fallback to captive portal AP mode
  - mDNS `p3a.local` hostname
  - NTP time synchronization
- **Kconfig**: `CONFIG_P3A_WIFI_MANAGER_ENABLE`, WiFi credentials

#### 4. **pico8** (PICO-8 Streaming)
- **Purpose**: PICO-8 game streaming over WebSocket
- **Files**: `pico8_stream.c`, `pico8_render.c`
- **Features**:
  - 128×128 indexed pixel frames
  - 16-color palette support
  - Nearest-neighbor upscaling to 720×720
  - Auto-timeout after 30 seconds
- **Kconfig**: `CONFIG_P3A_PICO8_ENABLE`, `CONFIG_P3A_PICO8_USB_STREAM_ENABLE`

#### 5. **animation_decoder** (Image Decoders)
- **Purpose**: Unified interface for image/animation decoding
- **Formats**: WebP (animated), GIF (animated), PNG, JPEG
- **Transparency**: Full alpha channel support for WebP, GIF, and PNG
- **Aspect ratio**: Preserves original aspect ratio when scaling non-square images
- **Files**: `webp_animation_decoder.c`, `png_animation_decoder.c`, `jpeg_animation_decoder.c`
- **Interface**: `animation_decoder.h` provides `decoder_open()`, `decoder_get_frame()`, `decoder_close()`

#### 6. **makapix** (Cloud Integration)
- **Purpose**: Makapix Club MQTT integration
- **Features**:
  - Device provisioning via HTTPS
  - TLS MQTT with mTLS authentication
  - **Artwork sending** — receive artworks directly from dev.makapix.club
  - Remote command receiving (swap_next, swap_back, etc.)
  - Status publishing every 30 seconds
- **Files**: `makapix.c`, `makapix_mqtt.c`, `makapix_provision.c`, `makapix_store.c`

#### 7. **http_api** (Web Interface)
- **Purpose**: HTTP server, REST API, WebSocket handler
- **Endpoints**: `/status`, `/action/*`, `/config/*`, `/files/*`, `/ws/pico8`
- **Features**: mDNS, static file serving, file upload/delete

#### 8. **config_store** (Configuration)
- **Purpose**: NVS-backed persistent configuration
- **Stores**: Wi-Fi credentials, brightness, auto-swap interval, rotation, background color

#### 9. **app_state** (State Machine)
- **Purpose**: Centralized application state (ready, processing, error)

#### 10. **ota_manager** (Over-the-Air Updates)
- **Purpose**: Wireless firmware updates from GitHub Releases
- **Features**:
  - Automatic periodic update checks (every 2 hours)
  - Web UI for manual check, install, and rollback
  - SHA256 checksum verification
  - Progress display on LCD during updates
  - Automatic rollback if firmware fails to boot 3 times
- **Files**: `ota_manager.c`, `ota_manager.h`

#### 11. **slave_ota** (ESP32-C6 Co-processor Firmware)
- **Purpose**: Automatic firmware management for the ESP32-C6 Wi-Fi co-processor
- **Features**:
  - Detects outdated or missing co-processor firmware
  - Automatically flashes ESP-Hosted firmware during boot
  - Progress display on LCD during flashing
- **Files**: `slave_ota.c`, `slave_ota.h`

### ESP Component Registry Dependencies

- **waveshare/esp32_p4_wifi6_touch_lcd_4b**: BSP for the hardware board
- **espressif/esp_lcd_touch**: Touch controller abstraction
- **espressif/libpng**: PNG decoding library
- **espressif/mdns**: mDNS responder
- **espressif/esp_tinyusb**: USB stack wrapper
- **espressif/esp_hosted**: ESP32-C6 hosted mode driver

---

## Hardware Interfaces

### Board: Waveshare ESP32-P4-WIFI6-Touch-LCD-4B

#### CPU and Memory
- **Main MCU**: Dual-core ESP32-P4 (RISC-V)
- **Wi-Fi/BLE MCU**: ESP32-C6-MINI-1 (managed by ESP-Hosted)
- **PSRAM**: 32MB external SDRAM
- **Flash**: 32MB NOR flash
- **Interconnect**: ESP-P4 ↔ ESP-C6 via SPI/SDIO

#### Display
- **Panel**: 720×720 pixel IPS LCD
- **Controller**: ST7703 (via MIPI-DSI)
- **Color depth**: RGB565 or RGB888 (configurable)
- **Frame buffer**: Multi-buffer (double/triple buffering)
- **Abstraction**: `p3a_board.h` provides `P3A_DISPLAY_*` constants

#### Touch Controller
- **Controller**: GT911 capacitive touch
- **Interface**: I²C
- **Features**: 5-point multitouch, gestures

#### Storage
- **Interface**: SDMMC (4-bit mode)
- **Mount point**: `/sdcard`
- **Functions**: `p3a_board_sdcard_mount()`, `p3a_board_sdcard_unmount()`

#### USB
- **Ports**: 2× USB-C (High-Speed used for composite device)
- **TinyUSB**: CDC-ACM + Mass Storage + Vendor bulk

---

## Network Stack

### Wi-Fi Management (wifi_manager component)

#### Architecture
- **Mode**: Station (STA) with AP fallback
- **Driver**: ESP-WiFi-Remote (offloads to ESP32-C6)
- **Stack**: LwIP TCP/IP

#### Provisioning Flow
1. **Boot**: Attempts to connect using saved NVS credentials
2. **Failure**: Starts captive portal AP mode (`p3a-setup`)
3. **Configuration**: User submits SSID/password via web form
4. **Reconnection**: Device saves credentials to NVS and reconnects

#### mDNS
- **Hostname**: `p3a.local`
- **Service**: `_http._tcp` advertised

### HTTP Server (http_api component)

#### REST API Endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/status` | Device status (JSON) |
| GET | `/config` | Current configuration |
| POST | `/config` | Update configuration |
| POST | `/action/swap_next` | Advance to next animation |
| POST | `/action/swap_back` | Go to previous animation |
| POST | `/action/reboot` | Reboot device |
| GET | `/files/list` | List SD card files |
| POST | `/files/upload` | Upload file to SD |

#### WebSocket
- **Endpoint**: `/ws/pico8`
- **Protocol**: Binary PICO-8 frames (palette + indexed pixels)

---

## Storage and Filesystem

### SPIFFS (Internal Flash)
- **Partition**: `storage` (1MB)
- **Mount point**: `/spiffs`
- **Initialization**: `p3a_board_spiffs_mount()` (in p3a_board_ep44b)
- **Purpose**: Web UI assets, configuration

### SD Card (SDMMC)
- **Mount point**: `/sdcard` (hardware level, BSP configured)
- **p3a root folder**: `/sdcard/p3a` (default, user-configurable via web UI)
- **Primary use**: Animation asset storage
- **User configuration**: Users specify folder name (e.g., `/p3a`, `/data`) in web UI
- **Internal handling**: `/sdcard` prefix is prepended automatically at runtime
- **Example**: User sets `/p3a` → System uses `/sdcard/p3a/animations`, `/sdcard/p3a/vault`, etc.
- **Reboot required**: Changes to root folder take effect after reboot
- **Supported formats**: `.webp`, `.gif`, `.png`, `.jpg/.jpeg`

### Vault Storage (channel_manager)
- **Layout**: `/sdcard/p3a/vault/ab/cd/<sha256>.<ext>` (SHA256-sharded)
- **Sidecar**: `.json` metadata files alongside assets
- **Atomic writes**: `.tmp` + `fsync` + `rename`

### SD Card Directory Structure
All p3a data is stored under the configurable root folder (`/sdcard/p3a` by default):
```
/sdcard/p3a/
├── animations/    # Local animation files (WebP, GIF, PNG, JPEG)
├── vault/         # Cached artwork from Makapix (sharded by SHA256)
├── channel/       # Channel settings and index files
├── playlists/     # Playlist cache files
└── downloads/     # Temporary upload storage
```

### NVS (Non-Volatile Storage)
- **Partition**: `nvs` (24KB)
- **Stores**: Wi-Fi credentials, device settings, playlist state

---

## Display Pipeline

### Initialization Flow
1. **Board Init**: `p3a_board_display_init()` via BSP
2. **Frame Buffers**: Allocated in PSRAM (720×720×3 bytes each)
3. **Display Renderer**: `display_renderer_init()` sets up vsync, buffers

### Rendering Pipeline

```
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│   Decoder    │────>│ display_     │────>│ Frame Buffer │
│ (WebP/GIF/   │     │ renderer     │     │  (PSRAM)     │
│  PNG/JPEG)   │     │ (upscale,    │     └──────┬───────┘
└──────────────┘     │  alpha blend)│            │
                     └──────────────┘            ▼
                                         ┌─────────────────┐
                                         │ LCD Panel DMA   │
                                         │ (MIPI-DSI)      │
                                         └─────────────────┘
```

### Rendering Features

- **Transparency/Alpha blending**: Images with transparent backgrounds are composited over a configurable background color
- **Aspect ratio preservation**: Non-square images are scaled to fit while maintaining original proportions
- **Configurable background**: Background color can be set via web UI or REST API
- **Nearest-neighbor scaling**: Preserves crisp pixel art edges during upscaling

### Key Files
- `display_renderer.c`: Frame buffer management, vsync, parallel upscaling, alpha blending
- `animation_player_render.c`: Frame decoding, aspect ratio calculation, composition
- `playback_controller.c`: Source switching (animation, PICO-8, UI)

---

## Configuration System

### Kconfig Structure

Configuration is organized into component-specific Kconfig files under a unified "P3A Features" menu:

- `main/Kconfig.projbuild`: General, Display, Animation, Touch, Wi-Fi, USB
- `components/pico8/Kconfig`: PICO-8 streaming options
- `components/p3a_board_ep44b/Kconfig`: Board-specific options
- `components/wifi_manager/Kconfig`: WiFi manager options
- `components/channel_manager/Kconfig`: Channel/playlist options
- `components/animation_decoder/Kconfig`: Decoder options

### Key Options

| Config | Default | Description |
|--------|---------|-------------|
| `P3A_AUTO_SWAP_INTERVAL_SECONDS` | 30 | Auto-swap timer |
| `P3A_MAX_SPEED_PLAYBACK` | yes | Ignore frame delays |
| `P3A_PICO8_ENABLE` | yes | Enable PICO-8 streaming |
| `P3A_USB_MSC_ENABLE` | yes | Enable USB Mass Storage |
| `P3A_PIXEL_FORMAT_RGB888` | yes | 24-bit color mode |

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

### Adding a New Component

1. Create directory under `components/`
2. Add `CMakeLists.txt` with `idf_component_register()`
3. Add `Kconfig` if configuration needed
4. Add component to main's REQUIRES in `main/CMakeLists.txt`

### Testing

Manual testing workflow:
1. Build verification: `idf.py build` succeeds
2. Flash verification: `idf.py flash` succeeds
3. Functional tests: Display, touch, Wi-Fi, USB

---

## Useful Links

- **Repository**: https://github.com/fabkury/p3a
- **Hardware**: [Waveshare ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/product/arduino/boards-kits/esp32-p4/esp32-p4-wifi6-touch-lcd-4b.htm)
- **ESP-IDF**: https://docs.espressif.com/projects/esp-idf/
- **Makapix Club**: https://makapix.club/

---

*Last updated: December 2025*
