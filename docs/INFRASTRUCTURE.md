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
- **Connectivity**: Wi-Fi, HTTP server, WebSocket, REST API, mDNS
- **I/O**: Capacitive touch, USB-C (CDC-ACM, Mass Storage, vendor bulk pipe)
- **Framework**: ESP-IDF v5.5.x

### Key Features

- Animated WebP, GIF, PNG, and JPEG playback
- Touch gestures for navigation and brightness control
- Web-based control panel at `http://p3a.local/`
- PICO-8 game monitor mode with WebSocket streaming
- USB Mass Storage for SD card access
- Captive portal for Wi-Fi provisioning

### Codebase Statistics

- **Total source files**: ~30 C/C++ files
- **Total lines of code**: ~10,000 lines
- **Components**: 6 custom components + multiple ESP-IDF registry dependencies
- **Build artifacts**: ~22MB (compiled binaries + SPIFFS image)

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                         p3a_main.c                           │
│                    (Application Entry Point)                 │
└──────────────────────┬──────────────────────────────────────┘
                       │
        ┌──────────────┼──────────────┬─────────────┐
        │              │              │             │
   ┌────▼─────┐  ┌────▼────┐   ┌────▼────┐   ┌───▼────┐
   │ app_lcd  │  │app_touch│   │app_wifi │   │app_usb │
   │  (LCD)   │  │ (Touch) │   │ (Wi-Fi) │   │ (USB)  │
   └────┬─────┘  └────┬────┘   └────┬────┘   └───┬────┘
        │             │              │            │
   ┌────▼──────────────▼─────────────▼────────────▼────┐
   │            animation_player.c                      │
   │         (Animation Playback Engine)                │
   └────┬───────────────────┬──────────────────────────┘
        │                   │
   ┌────▼─────┐      ┌─────▼──────────────┐
   │ Decoders │      │   Components       │
   │ WebP/GIF │      │ - app_state        │
   │ PNG/JPEG │      │ - config_store     │
   └──────────┘      │ - http_api         │
                     │ - libwebp_decoder  │
                     │ - animated_gif     │
                     └────────────────────┘
```

### Execution Flow

1. **Initialization** (`app_main`):
   - NVS flash initialization
   - Network interface setup
   - SPIFFS filesystem mount
   - LCD, touch, USB, and Wi-Fi initialization
   - HTTP server startup

2. **Runtime Operation**:
   - Animation player task renders frames to LCD
   - Touch task processes gestures
   - Auto-swap task cycles animations on timer
   - HTTP server handles REST API and WebSocket connections
   - USB tasks handle CDC-ACM and Mass Storage

---

## Directory Structure

```
p3a/
├── main/                      # Main application component
│   ├── p3a_main.c            # Application entry point
│   ├── app_lcd_p4.c          # LCD initialization and control
│   ├── app_touch.c           # Touch input handling
│   ├── app_wifi.c            # Wi-Fi and captive portal
│   ├── app_usb.c             # USB composite device
│   ├── animation_player.c    # Core animation engine
│   ├── animation_player_render.c    # Frame rendering
│   ├── animation_player_loader.c    # Asset loading
│   ├── animation_player_pico8.c     # PICO-8 mode
│   ├── pico8_stream.c        # WebSocket PICO-8 streaming
│   ├── fs_init.c             # SPIFFS initialization
│   ├── webp_animation_decoder.c     # WebP decoder
│   ├── png_animation_decoder.c      # PNG decoder
│   ├── jpeg_animation_decoder.c     # JPEG decoder
│   ├── usb_descriptors.c     # USB device descriptors
│   ├── CMakeLists.txt        # Main component build config
│   ├── Kconfig.projbuild     # Configuration menu items
│   └── include/              # Public headers
│
├── components/                # Custom components
│   ├── app_state/            # Application state management
│   ├── config_store/         # NVS-backed configuration
│   ├── http_api/             # HTTP server and REST API
│   ├── makapix/              # Makapix Club integration (MQTT, provisioning)
│   ├── libwebp_decoder/      # libwebp wrapper
│   └── animated_gif_decoder/ # Animated GIF decoder
│
├── webui/                     # Web interface files
│   ├── static/               # Static assets (CSS, JS, WASM)
│   │   ├── fake08.js         # PICO-8 emulator JS
│   │   ├── fake08.wasm       # PICO-8 emulator WebAssembly
│   │   ├── pico8.js          # PICO-8 interface logic
│   │   └── pico8.css         # PICO-8 interface styles
│   └── pico8/
│       └── index.html        # PICO-8 web interface
│
├── scripts/                   # Utility scripts
│   └── convert_pico8_logo.py # Asset conversion tool
│
├── def/                       # SDK configuration defaults
├── build/                     # Build output directory
│   ├── p3a.bin               # Main application binary
│   ├── storage.bin           # SPIFFS partition image
│   ├── bootloader/           # Bootloader binary
│   ├── partition_table/      # Partition table binary
│   ├── flash_args            # Flash command arguments
│   └── flasher_args.json     # Flasher configuration
│
├── managed_components/        # Auto-downloaded ESP-IDF components
├── CMakeLists.txt            # Root CMake configuration
├── partitions.csv            # Flash partition layout
├── sdkconfig                 # ESP-IDF configuration (generated)
├── dependencies.lock         # Component dependencies lockfile
├── README.md                 # User-facing documentation
├── ROADMAP.md                # Development roadmap
└── INFRASTRUCTURE.md         # This document
```

### Key Files

- **CMakeLists.txt**: Root build configuration, includes SPIFFS image creation
- **partitions.csv**: Defines flash memory layout (NVS, factory app, SPIFFS)
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

#### Partition Layout

From `partitions.csv`:

| Name      | Type | SubType | Offset | Size | Purpose |
|-----------|------|---------|--------|------|---------|
| nvs       | data | nvs     | 0x9000 | 24KB | Non-volatile storage |
| phy_init  | data | phy     | 0xf000 | 4KB  | PHY calibration |
| factory   | app  | factory | auto   | 8MB  | Application firmware |
| storage   | data | spiffs  | auto   | 7MB  | Web UI files |

**Total flash usage**: ~15MB of 32MB available

#### Component Registration

Main component (`main/CMakeLists.txt`):

```cmake
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include" "."
    REQUIRES driver libwebp_decoder waveshare__esp32_p4_wifi6_touch_lcd_4b 
             espressif__esp_lcd_touch animated_gif_decoder espressif__libpng 
             esp_driver_jpeg app_state config_store http_api esp_wifi 
             esp_wifi_remote esp_http_server mdns json nvs_flash esp_netif 
             esp_hosted tinyusb esp_tinyusb esp_driver_sdmmc spiffs
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

# Flash with specific port
idf.py -p /dev/ttyUSB0 flash monitor
```

### Prebuilt Binary Flashing

```bash
esptool.py --chip esp32p4 --before default_reset --after hard_reset \
  --flash_mode dio --flash_freq 80m --flash_size 32MB write_flash \
  0x2000 bootloader/bootloader.bin \
  0x8000 partition_table/partition-table.bin \
  0x10000 p3a.bin \
  0x810000 storage.bin
```

---

## Component Architecture

### Custom Components

#### 1. **app_state**
- **Purpose**: Centralized application state management
- **Files**: `app_state.c`, `app_state.h`
- **Dependencies**: FreeRTOS
- **Function**: Tracks device state (ready, processing, error)

#### 2. **config_store**
- **Purpose**: Persistent configuration storage
- **Files**: `config_store.c`, header in main directory
- **Dependencies**: nvs_flash, cJSON
- **Function**: Stores Wi-Fi credentials, device settings in NVS

#### 3. **http_api**
- **Purpose**: HTTP server, REST API, WebSocket handler
- **Files**: `http_api.c`, `http_api.h`, `favicon_data.h`, `pico8_logo_data.h`
- **Dependencies**: esp_http_server, mdns, cJSON, animation_player, app_lcd
- **Key features**:
  - REST endpoints: `/status`, `/action/*`, `/config/*`, `/files/*`
  - WebSocket: PICO-8 frame streaming
  - mDNS: `p3a.local` hostname
  - Static file serving from SPIFFS

#### 4. **libwebp_decoder**
- **Purpose**: WebP animation decoding wrapper
- **Dependencies**: External libwebp library
- **Function**: Decodes animated/static WebP images

#### 5. **animated_gif_decoder**
- **Purpose**: GIF animation decoding
- **Files**: `AnimatedGIF.cpp`, `gif_animation_decoder.c/cpp`, `gif.inl`
- **Dependencies**: None (vendored library)
- **Function**: Decodes animated/static GIF images

#### 6. **makapix**
- **Purpose**: Makapix Club integration (device registration, cloud connectivity)
- **Files**: 
  - `makapix.c/h`: State machine, provisioning orchestration, connection management
  - `makapix_mqtt.c/h`: TLS MQTT client with mutual TLS (mTLS) authentication
  - `makapix_provision.c/h`: HTTP provisioning to dev.makapix.club
  - `makapix_store.c/h`: Certificate and credential storage in NVS/SPIFFS
  - `makapix_certs.c/h`: CA certificate handling
  - `makapix_artwork.c/h`: Artwork metadata handling (future feed ingestion)
- **Dependencies**: esp_mqtt, esp_http_client, esp_crt_bundle, cJSON, nvs_flash, spiffs
- **Features**:
  - Device registration via HTTPS to `dev.makapix.club`
  - Registration code display for user verification
  - TLS MQTT with client certificate authentication (mTLS)
  - Status publishing every 30 seconds
  - Command receiving from website (swap_next, swap_back, etc.)
  - Automatic reconnection on connection loss
  - Secure certificate storage in SPIFFS

### ESP Component Registry Dependencies

From `dependencies.lock`:

- **waveshare/esp32_p4_wifi6_touch_lcd_4b**: BSP for the hardware board
- **espressif/esp_lcd_touch**: Touch controller abstraction
- **espressif/esp_lcd_touch_gt911**: GT911 touch driver
- **espressif/libpng**: PNG decoding library
- **espressif/zlib**: Compression library (required by libpng)
- **espressif/mdns**: mDNS responder for `p3a.local`
- **espressif/esp_tinyusb**: USB stack wrapper
- **espressif/tinyusb**: TinyUSB library
- **espressif/esp_wifi_remote**: Wi-Fi remote control for ESP32-C6
- **espressif/esp_hosted**: ESP32-C6 hosted mode driver
- **lvgl/lvgl**: Graphics library (used by BSP, not directly by app)

---

## Hardware Interfaces

### Board: Waveshare ESP32-P4-WIFI6-Touch-LCD-4B

#### CPU and Memory
- **Main MCU**: Dual-core ESP32-P4 (RISC-V)
- **Wi-Fi/BLE MCU**: ESP32-C6-MINI-1 (managed by ESP-Hosted)
- **PSRAM**: 32MB external SDRAM
- **Flash**: 32MB NOR flash
- **Interconnect**: ESP-P4 ↔ ESP-C6 via SPI/SDIO (ESP-Hosted protocol)

#### Display (app_lcd_p4.c)
- **Panel**: 720×720 pixel IPS LCD
- **Controller**: ST7703 (via MIPI-DSI)
- **Color depth**: RGB565 or RGB888 (configurable)
- **Backlight**: PWM-controlled LED backlight
- **Frame buffer**: Multi-buffer (double/triple buffering)
- **Pixel clock**: Configured via BSP

#### Touch Controller (app_touch.c)
- **Controller**: GT911 capacitive touch
- **Interface**: I²C
- **Features**: 5-point multitouch
- **Gestures supported**:
  - Tap (left/right half detection)
  - Vertical swipe (brightness control)
  - Long-press (reserved for future use)

#### Storage
- **Interface**: SDMMC (4-bit mode)
- **Mount point**: `/sdcard`
- **Format**: FAT32 (recommended)
- **File system**: VFS (Virtual File System)

#### USB (app_usb.c)
- **Ports**: 2× USB-C (only High-Speed port used for composite device)
- **TinyUSB configuration**:
  - CDC-ACM: Serial console
  - Mass Storage: SD card exposure
  - Vendor bulk: PICO-8 streaming (128×128 frames)
- **Descriptors**: Defined in `usb_descriptors.c`

#### GPIOs
- **LED**: Onboard red LED (GPIO assignment in BSP)
- **Expansion**: GPIO headers (not used by firmware)

---

## Network Stack

### Wi-Fi Management (app_wifi.c)

#### Architecture
- **Mode**: Station (STA) with AP fallback
- **Driver**: ESP-WiFi-Remote (offloads to ESP32-C6)
- **Stack**: LwIP TCP/IP

#### Provisioning Flow

1. **Boot**: Attempts to connect using saved NVS credentials
2. **Failure**: Starts captive portal AP mode
   - SSID: `p3a-setup` (configurable via `CONFIG_ESP_AP_SSID`)
   - IP: `192.168.4.1`
   - DNS: Captive portal redirects all requests to `/setup`
3. **Configuration**: User submits SSID/password via web form
4. **Reconnection**: Device saves credentials to NVS and reboots

#### mDNS (Multicast DNS)
- **Hostname**: `p3a.local`
- **Service**: `_http._tcp` advertised
- **Library**: espressif/mdns component

### HTTP Server (http_api.c)

#### Configuration
- **Port**: 80
- **Max connections**: Configurable (default 4)
- **WebSocket support**: Enabled (`CONFIG_HTTPD_WS_SUPPORT`)
- **Task stack**: 8KB (configurable)

#### REST API Endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/status` | Device status (JSON) |
| GET | `/config` | Current configuration |
| POST | `/config` | Update configuration |
| POST | `/action/swap_next` | Advance to next animation |
| POST | `/action/swap_back` | Go to previous animation |
| POST | `/action/pause` | Pause playback |
| POST | `/action/resume` | Resume playback |
| POST | `/action/reboot` | Reboot device |
| GET | `/files/list` | List SD card files |
| POST | `/files/upload` | Upload file to SD |
| DELETE | `/files/delete` | Delete file from SD |

#### Static Files
- **Source**: SPIFFS partition (`storage`)
- **Files**: Served from `/webui/` (compiled into `storage.bin`)
- **Favicon**: Embedded as C array (`favicon_data.h`)
- **PICO-8 assets**: JavaScript, WASM, CSS, HTML

#### WebSocket
- **Endpoint**: `/ws/pico8`
- **Protocol**: Binary frames (framebuffer + palette)
- **Frame format**:
  ```
  [0-5]: Magic + length + flags header
  [6-53]: 48-byte RGB palette (16 colors × 3 bytes)
  [54-8245]: 8192-byte indexed pixel data (128×128×8bpp)
  ```
- **Timeout**: 30 seconds of inactivity → exit PICO-8 mode

---

## Storage and Filesystem

### SPIFFS (SPI Flash File System)

**Partition**: `storage` (7MB)

#### Purpose
- Stores web UI assets (HTML, CSS, JS, WASM)
- Read-only at runtime
- Built from `webui/` directory during compilation

#### Initialization (fs_init.c)
```c
esp_vfs_spiffs_conf_t conf = {
    .base_path = "/spiffs",
    .partition_label = "storage",
    .max_files = 5,
    .format_if_mount_failed = false
};
esp_vfs_spiffs_register(&conf);
```

### SD Card (SDMMC)

**Mount point**: `/sdcard`

#### Usage
- **Primary**: Animation asset storage
- **Preferred directory**: `/sdcard/animations`
- **Fallback**: Auto-scan entire card for animation files
- **Supported formats**: `.webp`, `.gif`, `.png`, `.jpg/.jpeg`

#### Access Modes
1. **Normal**: Animation player reads files
2. **USB Mass Storage**: Exposes SD to PC (animation playback paused)

#### File Operations
- **Scan**: Recursive directory traversal
- **Load**: Memory-mapped or streamed decoding
- **Upload**: Via HTTP `/files/upload` endpoint

### NVS (Non-Volatile Storage)

**Partition**: `nvs` (24KB)

#### Stored Data
- Wi-Fi credentials (SSID, password)
- Device configuration (brightness, auto-swap interval)
- Playlist state (last played index)
- Custom settings from menuconfig

#### API Usage
```c
nvs_handle_t handle;
nvs_open("config", NVS_READWRITE, &handle);
nvs_set_str(handle, "ssid", ssid);
nvs_commit(handle);
nvs_close(handle);
```

---

## Display Pipeline

### Initialization (app_lcd_p4.c)

#### Flow
1. **BSP Init**: Waveshare BSP handles pin muxing, clocks
2. **Panel Init**: ST7703 MIPI-DSI initialization sequence
3. **Backlight**: PWM channel setup (default: 50% brightness)
4. **Frame Buffers**: Allocate 2-3 buffers in PSRAM (configurable)

#### Frame Buffer
- **Size**: 720×720 pixels × 2 bytes (RGB565) = 1.035MB per buffer
- **Location**: External PSRAM (cached)
- **Count**: 2 (double-buffering) or 3 (triple-buffering)
- **Format**: RGB565 (default) or RGB888 (configurable in menuconfig)

### Rendering Pipeline (animation_player_render.c)

```
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│   Decoder    │────>│  Scaler &    │────>│ Frame Buffer │
│ (WebP/GIF/   │     │  Compositor  │     │  (PSRAM)     │
│  PNG/JPEG)   │     └──────────────┘     └──────┬───────┘
└──────────────┘                                   │
                                                   ▼
                                          ┌─────────────────┐
                                          │ esp_cache_msync │
                                          │ (flush to RAM)  │
                                          └────────┬────────┘
                                                   ▼
                                          ┌─────────────────┐
                                          │ LCD Panel DMA   │
                                          │ (MIPI-DSI)      │
                                          └─────────────────┘
```

#### Rendering Steps
1. **Decode**: Extract frame from WebP/GIF/PNG/JPEG
2. **Scale**: Nearest-neighbor upscale to 720×720 (if source < 720)
3. **Composite**: Center artwork, fill borders with black
4. **Cache Flush**: `esp_cache_msync()` if `CONFIG_P3A_LCD_ENABLE_CACHE_FLUSH`
5. **DMA Transfer**: `esp_lcd_panel_draw_bitmap()`

#### Brightness Control
- **Method**: PWM duty cycle on backlight pin
- **Range**: 0-100%
- **Gesture**: Vertical swipe modulates brightness proportionally
- **Formula**: `Δbrightness = swipe_distance × CONFIG_P3A_TOUCH_BRIGHTNESS_MAX_DELTA_PERCENT / screen_height`

### Animation Playback (animation_player.c)

#### Task Structure
- **Task name**: `anim_player`
- **Priority**: `CONFIG_P3A_RENDER_TASK_PRIORITY` (default: 5)
- **Stack**: 8KB
- **Core affinity**: None (FreeRTOS scheduler decides)

#### Playback Modes
1. **Frame-timed**: Respect original animation delays (if `!CONFIG_P3A_MAX_SPEED_PLAYBACK`)
2. **Max speed**: Render frames as fast as possible (default)
3. **Static images**: Use `CONFIG_P3A_STATIC_FRAME_DELAY_MS` (default: 100ms)

#### Asset Loading (animation_player_loader.c)
- **Directory scan**: Build playlist from `/sdcard/animations` (or fallback search)
- **Sorting**: Alphabetical by filename
- **Caching**: Keep current/next/previous decoded in memory
- **Prefetching**: Background task loads adjacent animations

---

## Touch Input System

### Driver (app_touch.c)

#### GT911 I²C Configuration
- **Address**: 0x5D or 0x14 (auto-detected by BSP)
- **Interrupt**: GPIO (BSP-defined)
- **Polling interval**: `CONFIG_P3A_TOUCH_POLL_INTERVAL_MS` (default: 20ms)

### Gesture Recognition

#### Tap Detection
- **Left half tap**: Previous animation (`x < 360`)
- **Right half tap**: Next animation (`x >= 360`)
- **Tap duration**: < 500ms (no drag movement)

#### Vertical Swipe
- **Minimum distance**: `CONFIG_P3A_TOUCH_SWIPE_MIN_HEIGHT_PERCENT` (default: 10% of 720px)
- **Direction**:
  - **Up**: Increase brightness
  - **Down**: Decrease brightness
- **Max delta**: `CONFIG_P3A_TOUCH_BRIGHTNESS_MAX_DELTA_PERCENT` (default: 75%)

#### Long Press
- **Duration**: > 500ms
- **Current use**: Reserved for "like" reaction (future Makapix Club integration)

### Task Architecture
- **Task name**: `touch_task`
- **Priority**: `CONFIG_P3A_TOUCH_TASK_PRIORITY` (default: 5)
- **Stack**: 4KB
- **Function**: Polls GT911, dispatches to handlers, resets auto-swap timer

---

## USB Functionality

### Composite Device (app_usb.c)

#### TinyUSB Configuration
- **Mode**: High-Speed device (480 Mbps)
- **Configuration**: Composite device with 3 interfaces
- **Port**: USB-C High-Speed (FS port reserved for bootloader)

#### Interface Descriptions

##### 1. CDC-ACM (Serial Console)
- **Interface**: 0 (control) + 1 (data)
- **Endpoints**: IN/OUT bulk + IN interrupt (notify)
- **Baud rate**: Virtual (ignored, USB full-speed)
- **Purpose**: Serial logging, console access
- **Connected to**: `stdout`, `stdin`, ESP_LOG output

##### 2. Mass Storage Class (MSC)
- **Interface**: 2
- **Endpoints**: IN/OUT bulk
- **LUN**: 0 (Logical Unit 0 = SD card)
- **Block size**: 512 bytes
- **Capacity**: SD card size
- **Purpose**: Access SD card files from PC/phone
- **Behavior**: Locks SD card (playback continues with current animation)

##### 3. Vendor Bulk Interface
- **Interface**: 3
- **Endpoints**: IN bulk
- **Purpose**: PICO-8 frame streaming (128×128 pixel data)
- **Protocol**: Custom (not currently used; WebSocket preferred)

### USB Descriptors (usb_descriptors.c)

```c
Device Descriptor:
  VID: 0x303A (Espressif)
  PID: 0x4003 (p3a custom)
  Class: 0xEF (Miscellaneous)
  Subclass: 0x02 (Common Class)
  Protocol: 0x01 (Interface Association Descriptor)
  
Configuration Descriptor:
  - Interface Association (CDC)
  - CDC Control Interface
  - CDC Data Interface
  - Mass Storage Interface
  - Vendor Bulk Interface
```

---

## Web UI and REST API

### Architecture

#### Static Web Interface
- **Location**: `/spiffs/` (served from SPIFFS partition)
- **Entry point**: `/pico8/index.html`
- **Assets**:
  - `fake08.js`, `fake08.wasm`: PICO-8 emulator (WebAssembly)
  - `pico8.js`: WebSocket communication, cart loading
  - `pico8.css`: Styling

#### Dynamic REST API
- **Handler**: `http_api.c` registers URI handlers
- **Response format**: JSON (via cJSON library)
- **Authentication**: None (local network only)

### Endpoints Detail

#### GET /status
**Response**:
```json
{
  "state": "ready",
  "animation": {
    "index": 0,
    "count": 10,
    "paused": false
  },
  "wifi": {
    "ssid": "MyNetwork",
    "ip": "192.168.1.100"
  },
  "uptime": 12345,
  "heap_free": 500000
}
```

#### POST /action/swap_next
**Response**: `{"result": "ok"}`

#### POST /action/reboot
**Response**: `{"result": "rebooting"}` (then device restarts)

#### GET /files/list?path=/sdcard/animations
**Response**:
```json
{
  "files": [
    {"name": "art1.webp", "size": 102400, "type": "file"},
    {"name": "art2.gif", "size": 204800, "type": "file"}
  ]
}
```

### WebSocket: PICO-8 Streaming

#### Connection Flow
1. **Client**: Opens `ws://p3a.local/ws/pico8`
2. **Server**: Accepts, enters PICO-8 mode (pauses animation player)
3. **Client**: Loads PICO-8 cart in fake-08 emulator (WASM)
4. **Client**: Sends frames at 30 FPS:
   ```
   Binary frame = [magic][len][flags][48-byte palette][8192-byte pixels]
   ```
5. **Server**: Decodes, upscales 128×128 → 720×720, renders to LCD
6. **Timeout**: After 30 seconds of no frames, exits PICO-8 mode

#### Frame Format
- **Magic**: `0x50 0x38` ("P8")
- **Length**: uint32_t (total frame size)
- **Flags**: uint8_t (reserved)
- **Palette**: 16 colors × 3 bytes (RGB)
- **Pixels**: 128×128 indexed pixels (16,384 bytes, palette indices)

### mDNS Discovery
- **Service**: `_http._tcp`
- **Hostname**: `p3a.local`
- **Port**: 80
- **TXT records**: None

---

## Configuration System

### menuconfig Options (Kconfig.projbuild)

#### General
- `P3A_AUTO_SWAP_INTERVAL_SECONDS`: Auto-swap timer (default: 30s)
- `P3A_STATIC_FRAME_DELAY_MS`: Frame delay for static images (default: 100ms)

#### Display
- `P3A_LCD_ENABLE_CACHE_FLUSH`: Enable cache flush before DMA (default: yes)

#### Animation
- `P3A_SD_ANIMATIONS_DIR`: Animation directory (default: `/sdcard/animations`)
- `P3A_MAX_SPEED_PLAYBACK`: Ignore frame delays, play at max speed (default: yes)
- `P3A_RENDER_TASK_PRIORITY`: Render task priority (default: 5)

#### Touch
- `P3A_TOUCH_TASK_PRIORITY`: Touch task priority (default: 5)
- `P3A_TOUCH_POLL_INTERVAL_MS`: Polling rate (default: 20ms)
- `P3A_TOUCH_SWIPE_MIN_HEIGHT_PERCENT`: Minimum swipe distance (default: 10%)
- `P3A_TOUCH_BRIGHTNESS_MAX_DELTA_PERCENT`: Max brightness change (default: 75%)

#### PICO-8
- `P3A_PICO8_ENABLE`: Enable PICO-8 monitor feature (default: no, disabled to reduce binary size)
- `P3A_PICO8_USB_STREAM_ENABLE`: Enable USB PICO-8 streaming (requires PICO-8 enabled)
- `P3A_PICO8_WEBSOCKET_TIMEOUT_SEC`: WebSocket inactivity timeout (default: 30s)

#### Wi-Fi
- `ESP_AP_SSID`: Captive portal SSID (default: `p3a-setup`)
- `ESP_AP_PASSWORD`: AP password (default: empty)

### Runtime Configuration (config_store)

Stored in NVS:
- Wi-Fi credentials (SSID, password)
- Last animation index
- Brightness level
- User preferences (future)

---

## Development Workflow

### Prerequisites

1. **ESP-IDF**: v5.5.x ([installation guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/))
2. **Python**: 3.8+ (for idf.py tools)
3. **Toolchain**: RISC-V GCC (auto-installed by ESP-IDF)
4. **Hardware**: Waveshare ESP32-P4-WIFI6-Touch-LCD-4B + microSD card

### Setup

```bash
# Clone repository
git clone https://github.com/fabkury/p3a.git
cd p3a

# Activate ESP-IDF environment
. $HOME/esp/esp-idf/export.sh

# Set target
idf.py set-target esp32p4

# Pull dependencies (automatic on first build)
idf.py reconfigure
```

### Build and Flash

```bash
# Full build
idf.py build

# Flash bootloader, partition table, app, and SPIFFS
idf.py flash

# Monitor serial output
idf.py monitor

# All-in-one: build, flash, monitor
idf.py build flash monitor
```

### Debugging

```bash
# Enable verbose logging
idf.py menuconfig
# → Component config → Log output → Default log verbosity → Verbose

# View logs
idf.py monitor

# Filter logs by tag
idf.py monitor | grep "HTTP"
```

### Modifying Web UI

1. Edit files in `webui/static/` or `webui/pico8/`
2. Rebuild (SPIFFS image regenerates automatically):
   ```bash
   idf.py build
   ```
3. Flash only SPIFFS partition:
   ```bash
   esptool.py --chip esp32p4 write_flash 0x810000 build/storage.bin
   ```

### Adding a New Animation Decoder

1. Create decoder in `main/` (e.g., `apng_decoder.c`)
2. Implement interface from `animation_decoder.h`
3. Register in `animation_player_loader.c`:
   ```c
   if (ends_with(filename, ".apng")) {
       return apng_decode_open(filename, &s_current_decoder);
   }
   ```
4. Add to `main/CMakeLists.txt`:
   ```cmake
   list(APPEND srcs "apng_decoder.c")
   ```

### Testing

Currently, there are no automated tests. Manual testing workflow:

1. **Build verification**: `idf.py build` succeeds
2. **Flash verification**: `idf.py flash` succeeds
3. **Boot verification**: Serial output shows no errors
4. **Functional tests**:
   - Display shows animations
   - Touch gestures work (tap, swipe)
   - Wi-Fi connects
   - Web UI loads at `http://p3a.local/`
   - PICO-8 mode streams correctly
   - USB mass storage exposes SD card

### Linting

No linter is currently configured. Code follows ESP-IDF style guide:
- Indentation: 4 spaces
- Naming: `snake_case` for functions/variables, `UPPER_CASE` for macros
- Headers: Include guards, C++ compatibility

### Performance Profiling

```bash
# Enable profiling
idf.py menuconfig
# → Component config → FreeRTOS → Enable task statistics

# View task stats
# In code:
vTaskGetRunTimeStats(buf);
ESP_LOGI(TAG, "\n%s", buf);
```

---

## Current Infrastructure Status

Implemented features:

1. **TLS MQTT Client**: Device registration and secure connection to Makapix Club (implemented)
2. **Remote Control**: Website can send commands to device via MQTT (implemented)
3. **Status Publishing**: Device reports status every 30 seconds (implemented)

## Future Infrastructure Improvements

Planned enhancements from ROADMAP.md:

1. **Feed Ingestion**: Download artworks automatically from MQTT notifications
2. **Reactions**: Send likes to artworks from the device hardware
3. **OTA Updates**: Firmware updates with rollback
4. **Playlists**: JSON-based playlist management
5. **Watchdog**: Task health monitoring
6. **Diagnostics**: Crash dumps, heap tracking
7. **Web Flasher**: Browser-based firmware flashing (under development)
8. **CI/CD**: Automated builds and releases
9. **Unit Tests**: Component-level testing framework

---

## Useful Links

- **Repository**: https://github.com/fabkury/p3a
- **Hardware**: [Waveshare ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/product/arduino/boards-kits/esp32-p4/esp32-p4-wifi6-touch-lcd-4b.htm)
- **ESP-IDF**: https://docs.espressif.com/projects/esp-idf/
- **Makapix Club**: https://makapix.club/

---

*This document was generated for p3a firmware. Last updated: December 2025*
