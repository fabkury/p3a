# Build System

## Framework: ESP-IDF 5.5.x

The project uses **ESP-IDF** (Espressif IoT Development Framework) with CMake as the build system.

- **Target**: `esp32p4`
- **Compiler flags**: `-Wno-ignored-qualifiers` to suppress qualifier warnings
- **LittleFS**: Web UI assets compiled into a flash partition image

## Root CMakeLists.txt

The root `CMakeLists.txt` manages three independent version numbers and the build pipeline:

```cmake
cmake_minimum_required(VERSION 3.16)

# Firmware version (semantic versioning, embedded in binary, used by OTA)
set(PROJECT_VER "0.8.4-dev")

# Web UI version (X.Y format, independent from firmware, auto-updated during OTA)
set(WEBUI_VERSION "1.3")

# API version (bump only for breaking HTTP API changes)
set(P3A_API_VERSION 2)

# Optional: Windows flasher executable build
set(P3A_BUILD_FLASHER OFF)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
add_compile_options(-Wno-ignored-qualifiers)
project(p3a)
```

Key features beyond the boilerplate:

- **Version parsing**: `PROJECT_VER` is split into `FW_VERSION_MAJOR`/`MINOR`/`PATCH` and cached so all components can access them
- **Web UI metadata**: Generates `webui/version.txt` and `webui/metadata.json` at configure time
- **Conditional LittleFS**: When PICO-8 is disabled (`CONFIG_P3A_PICO8_ENABLE=n`), PICO-8 assets are filtered out to reduce binary size
- **Release packaging**: Post-build step copies binaries to `release/v{version}/`, generates SHA256 checksums, flash args, and `manifest.json` for OTA
- **Flasher build**: Optional Windows flasher executable with embedded firmware (`P3A_BUILD_FLASHER=ON`)

## Main Component Registration

From `main/CMakeLists.txt`:

```cmake
set(srcs
    "app_lcd_p4.c"
    "app_touch.c"
    "p3a_main.c"
    "display_renderer.c"
    "display_fps_overlay.c"
    "display_processing_notification.c"
    "display_upscaler.c"
    "render_engine.c"
    "content_service.c"
    "playback_service.c"
    "connectivity_service.c"
    "animation_player.c"
    "animation_player_render.c"
    "animation_player_loader.c"
    "playback_controller.c"
    "ugfx_ui.c"
)

# Conditional: PPA hardware upscaling
if(CONFIG_P3A_PPA_UPSCALE_ENABLE)
    list(APPEND srcs "display_ppa_upscaler.c")
endif()

# Conditional: USB composite device
if(CONFIG_P3A_USB_MSC_ENABLE)
    list(APPEND srcs "app_usb.c" "usb_descriptors.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include" "."
    REQUIRES driver animation_decoder p3a_board_ep44b
             waveshare__esp32_p4_wifi6_touch_lcd_4b espressif__esp_lcd_touch
             config_store http_api json nvs_flash esp_netif esp_hosted
             tinyusb esp_tinyusb esp_driver_sdmmc makapix ugfx
             ota_manager slave_ota channel_manager wifi_manager pico8
             p3a_core sdio_bus debug_http_log play_scheduler event_bus
             content_cache playback_queue loader_service show_url
             giphy esp_driver_ppa
    PRIV_REQUIRES esp_mm
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    FW_VERSION_STRING="${PROJECT_VER}"
    FW_VERSION_MAJOR=${FW_VERSION_MAJOR}
    FW_VERSION_MINOR=${FW_VERSION_MINOR}
    FW_VERSION_PATCH=${FW_VERSION_PATCH}
    P3A_API_VERSION=${P3A_API_VERSION}
)
```

## Flash Partition Layout

From `partitions.csv`:

| Name | Type | SubType | Offset | Size | Purpose |
|------|------|---------|--------|------|---------|
| `nvs` | data | nvs | 0x9000 | 24 KB | Configuration storage |
| `phy_init` | data | phy | 0xF000 | 4 KB | PHY initialization data |
| `otadata` | data | ota | 0x10000 | 8 KB | OTA boot slot selection and rollback state |
| `ota_0` | app | ota_0 | 0x20000 | 8 MB | Primary OTA app slot |
| `ota_1` | app | ota_1 | 0x820000 | 8 MB | Secondary OTA app slot |
| `storage` | data | spiffs | 0x1020000 | 4 MB | LittleFS web UI assets |
| `slave_fw` | data | 0x40 | 0x1420000 | 2 MB | ESP32-C6 co-processor firmware |

## Build Commands

```bash
# Set target (first time only)
idf.py set-target esp32p4

# Configure project
idf.py menuconfig

# Build project
idf.py build

# Flash and monitor
idf.py flash monitor

# Clean build
idf.py fullclean
```

## ESP Component Registry Dependencies

- **waveshare/esp32_p4_wifi6_touch_lcd_4b**: BSP for the hardware board
- **espressif/esp_lcd_touch**: Touch controller abstraction
- **espressif/libpng**: PNG decoding library
- **espressif/mdns**: mDNS responder
- **espressif/esp_tinyusb**: USB stack wrapper
- **espressif/esp_hosted**: ESP32-C6 hosted mode driver
