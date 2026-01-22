# Memory Allocation Findings

This document provides a comprehensive inventory of memory allocations in the p3a codebase, categorized by allocation method and size.

## 1. heap_caps_malloc() Allocations (ESP-IDF Aware)

These allocations explicitly specify memory capabilities.

### Already Using PSRAM

| File | Line | Size | Flags | Purpose |
|------|------|------|-------|---------|
| `components/loader_service/loader_service.c` | 40 | file_size | `SPIRAM \| 8BIT` | Animation file data (with malloc fallback) |
| `components/ota_manager/github_ota.c` | 223 | 128 KB | `SPIRAM \| 8BIT` | GitHub API response buffer |
| `components/ota_manager/github_ota.c` | 410 | 256 B | `SPIRAM \| 8BIT` | SHA256 checksum response |
| `components/makapix/makapix_artwork.c` | 226 | 128 KB | `SPIRAM` | Artwork download chunk buffer |
| `components/makapix/makapix_mqtt.c` | 173 | 128 KB | `SPIRAM \| 8BIT` | MQTT message reassembly buffer |
| `components/makapix/makapix_mqtt.c` | 310 | variable | `SPIRAM \| 8BIT` | MQTT API response payload copy |
| `components/makapix/makapix_provision.c` | 226 | 16 KB | `SPIRAM \| 8BIT` | Credentials HTTP response |
| `components/pico8/pico8_render.c` | 103 | 64 KB x2 | `SPIRAM \| 8BIT` | PICO-8 frame buffers (double buffered) |
| `components/channel_manager/makapix_channel_impl.c` | 491 | 48 KB | `SPIRAM \| 8BIT` | Refresh task stack (pre-allocated) |

### Using Internal RAM (Intentionally)

| File | Line | Size | Flags | Purpose |
|------|------|------|-------|---------|
| `main/animation_player_loader.c` | 674 | 1.4 KB | `INTERNAL` | Upscale lookup table X |
| `main/animation_player_loader.c` | 681 | 1.4 KB | `INTERNAL` | Upscale lookup table Y |
| `components/pico8/pico8_render.c` | 144 | 1.4 KB | `INTERNAL` | PICO-8 upscale lookup X |
| `components/pico8/pico8_render.c` | 154 | 1.4 KB | `INTERNAL` | PICO-8 upscale lookup Y |

### Using DMA-Capable Memory

| File | Line | Size | Flags | Purpose |
|------|------|------|-------|---------|
| `components/ota_manager/ota_manager.c` | 568 | 4 KB | `DMA \| 8BIT` | Partition read buffer for SHA256 |

---

## 2. Standard malloc()/calloc() Allocations

These allocations use the default heap, which with `CONFIG_SPIRAM_USE_MALLOC=y` will:
- Stay in internal RAM if size < 16384 bytes (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL)
- Go to PSRAM if size >= 16384 bytes

### Large Allocations (>= 16KB, Should Auto-Use PSRAM)

| File | Line | Size | Purpose | Notes |
|------|------|------|---------|-------|
| `main/animation_player_loader.c` | 783 | canvas_w * canvas_h * 3-4 | Native frame buffer B1 | Up to 2MB for 720x720 RGBA |
| `main/animation_player_loader.c` | 790 | canvas_w * canvas_h * 3-4 | Native frame buffer B2 | Up to 2MB for 720x720 RGBA |
| `components/channel_manager/channel_cache.c` | 247 | entry_count * 64 | Cache entries array | Up to 64KB for 1024 entries |
| `components/channel_manager/channel_cache.c` | 266 | entry_count * 4 | Available post IDs array | Up to 4KB for 1024 entries |
| `components/channel_manager/channel_cache.c` | 336 | file_size | Binary cache file data | Variable |
| `components/channel_manager/channel_cache.c` | 662 | serialization buffer | Cache write buffer | Variable |
| `components/makapix/makapix_channel_refresh.c` | 378 | downloaded_count * 64 | Downloaded entries temp array | Variable |
| `components/play_scheduler/play_scheduler_cache.c` | 122 | count * sizeof(index_entry) | SD card index entries | Variable |

### Medium Allocations (1KB - 16KB, Stay in Internal RAM)

| File | Line | Size | Purpose |
|------|------|------|---------|
| `components/makapix/makapix.c` | 371-373 | 4 KB x3 | TLS certificate buffers (CA, cert, key) |
| `components/http_api/http_api_upload.c` | 146 | ~4 KB | HTTP upload chunk buffer |
| `components/wifi_manager/app_wifi.c` | 801, 827 | several KB | HTML page generation buffers |
| `components/channel_manager/channel_settings.c` | 33 | file_size | Settings JSON file |
| `main/animation_player.c` | (various) | 16 KB | Animation SD refresh task buffer |

### Small Allocations (< 1KB, Stay in Internal RAM)

Many small allocations for strings, paths, metadata, hash table nodes, etc. These are not migration candidates.

---

## 3. FreeRTOS Task Stack Allocations

Task stacks are allocated from internal RAM by default when using `xTaskCreate()`.

### Large Task Stacks (Migration Candidates)

| Task Name | File | Line | Stack Size | Notes |
|-----------|------|------|------------|-------|
| `download_mgr` | `components/channel_manager/download_manager.c` | 592 | **80 KB** | Largest stack - for SHA256, mbedTLS, newlib printf |
| `anim_sd_refresh` | `main/animation_player.c` | 616 | 16 KB | Animation SD export |
| `cred_poll` | `components/makapix/makapix_provision_flow.c` | 61 | 16 KB | Credential polling |
| `mqtt_reconn` | `components/makapix/makapix.c` | 432 | 16 KB | MQTT reconnection |

### Medium Task Stacks

| Task Name | File | Line | Stack Size |
|-----------|------|------|------------|
| `anim_loader` | `main/animation_player.c` | 321 | 8 KB |
| `app_touch_task` | `main/app_touch.c` | 502 | 8 KB |
| `ch_switch` | `components/makapix/makapix.c` | 156 | 8 KB |
| `makapix_prov` | `components/makapix/makapix.c` | 242 | 8 KB |
| `ota_check` | `components/ota_manager/ota_manager.c` | 547 | 8 KB |

### Already Using PSRAM Stack

| Task Name | File | Notes |
|-----------|------|-------|
| `makapix_refresh` | `components/channel_manager/makapix_channel_impl.c` | Uses `xTaskCreateStatic` with pre-allocated PSRAM buffer |

---

## 4. Display/DMA Buffers (Managed by ESP-IDF)

These are allocated by the ESP-IDF drivers, not directly by p3a code.

| Buffer Type | Size | Memory Region | Managed By |
|-------------|------|---------------|------------|
| LCD Frame Buffer 0 | 1.48 MB | DMA-capable SRAM | ESP-IDF MIPI-DSI driver |
| LCD Frame Buffer 1 | 1.48 MB | DMA-capable SRAM | ESP-IDF MIPI-DSI driver |
| LCD Frame Buffer 2 | 1.48 MB | DMA-capable SRAM | ESP-IDF MIPI-DSI driver |

Note: BSP configures `CONFIG_BSP_LCD_DPI_BUFFER_NUMS=3` for triple buffering.

---

## 5. Decoder-Specific Buffers

### JPEG Decoder

| File | Line | Size | Notes |
|------|------|------|-------|
| `components/animation_decoder/jpeg_animation_decoder.c` | 88-104 | width * height * 3 | Uses `jpeg_alloc_decoder_mem()` (DMA-capable) |

### PNG Decoder

| File | Line | Size | Notes |
|------|------|------|-------|
| `components/animation_decoder/png_animation_decoder.c` | 171-186 | width * height * 3-4 | Uses `malloc()` |
| `components/animation_decoder/png_animation_decoder.c` | 218 | height * 8 | Row pointers |

### WebP Decoder

Uses libwebp's internal memory management via `esp_webp_malloc` hooks.

### GIF Decoder

| File | Line | Size | Notes |
|------|------|------|-------|
| `components/animated_gif_decoder/gif_animation_decoder.cpp` | 233 | width * height * 3 | Canvas RGB buffer |

---

## 6. Total Memory Estimates by Category

### Internal RAM Usage (Approximate)

| Category | Estimated Size | Notes |
|----------|----------------|-------|
| Task Stacks | ~200 KB | Sum of all task stacks |
| Upscale Lookup Tables | ~6 KB | Intentionally internal |
| Small allocations (<16KB) | ~50-100 KB | Strings, paths, metadata |
| WiFi/LWIP buffers | ~100-200 KB | Managed by ESP-IDF |
| Display DMA buffers | ~4.4 MB | 3 frame buffers |

### PSRAM Usage (Approximate)

| Category | Estimated Size | Notes |
|----------|----------------|-------|
| Animation file data | Variable (1-5 MB) | Loaded files |
| Animation frame buffers | ~3-4 MB | Double buffered decode |
| Network buffers | ~400 KB | MQTT, downloads, etc. |
| Channel cache | ~100 KB | Entries and metadata |
| PICO-8 buffers | ~128 KB | If PICO-8 enabled |
