# Component Architecture

All 24 custom components live under `components/`. This document describes each one.

---

## 1. p3a_core — Unified State Machine and Lifecycle

- **Purpose**: Central state machine, touch routing, rendering dispatch, SD path management, and boot utilities
- **Key files**: `p3a_state.c`, `p3a_render.c`, `p3a_touch_router.c`, `sd_path.c`, `p3a_boot_logo.c`, `p3a_logo.c`, `fresh_boot.c`
- **Public API**: `p3a_state.h`, `p3a_render.h`, `p3a_touch_router.h`, `sd_path.h`, `p3a_boot_logo.h`, `fresh_boot.h`
- **States**: `ANIMATION_PLAYBACK`, `PROVISIONING`, `OTA`, `PICO8_STREAMING`
- **Key functions**:
  - `p3a_state_init()`, `p3a_state_get()`, `p3a_state_switch_channel()`, `p3a_state_enter_*()` — state transitions
  - `p3a_touch_router_init()`, `p3a_touch_router_handle_event()` — state-aware touch routing
  - `p3a_render_init()`, `p3a_render_frame()` — state-aware rendering dispatch
  - `sd_path_init()`, `sd_path_get_*()` — configurable SD card root directory (default `/sdcard/p3a`)

## 2. play_scheduler — Deterministic Multi-Channel Playback Engine

- **Purpose**: Executes playsets (scheduler commands) to select artwork across multiple channels using Smooth Weighted Round Robin (SWRR)
- **Key files**: `play_scheduler.c`, `play_scheduler_swrr.c`, `play_scheduler_commands.c`, `play_scheduler_pick.c`, `play_scheduler_navigation.c`, `play_scheduler_timer.c`, `play_scheduler_nae.c`, `play_scheduler_lai.c`, `play_scheduler_refresh.c`, `play_scheduler_buffers.c`, `play_scheduler_cache.c`, `playset_store.c`, `playset_json.c`
- **Public API**: `play_scheduler.h`, `play_scheduler_types.h`, `playset_store.h`, `playset_json.h`
- **Key concepts**:
  - **Playset** (`ps_scheduler_command_t`): Specifies which channels to include, exposure balance, and pick mode
  - **Exposure modes**: Equal Exposure (EqE), Maximum Exposure (MaE), Priority Exposure (PrE)
  - **Pick modes**: Recency-based, Random
  - **NAE**: New Artwork Events — react to freshly downloaded content
  - **LAi**: Last Access Index — persistence of playback position per channel
- **Key functions**: `play_scheduler_init()`, `play_scheduler_execute_command()`, `play_scheduler_next()`, `play_scheduler_prev()`, `play_scheduler_peek_next()`, `play_scheduler_play_named_channel()`, `play_scheduler_play_artwork()`
- **Kconfig**: `components/play_scheduler/Kconfig`

## 3. event_bus — Asynchronous Event Pub/Sub

- **Purpose**: Decoupled component communication via asynchronous events
- **Key files**: `event_bus.c`
- **Public API**: `event_bus.h`
- **Event categories**: `SYSTEM`, `CONTENT`, `PLAYBACK`, `UI`
- **Event types**: WiFi/MQTT connectivity changes, cache flush, swap/pause/resume, provisioning status changes
- **Key functions**: `event_bus_init()`, `event_bus_subscribe()`, `event_bus_subscribe_category()`, `event_bus_emit()`, `event_bus_emit_simple()`, `event_bus_emit_i32()`, `event_bus_emit_ptr()`, `event_bus_unsubscribe()`

## 4. p3a_board_ep44b — Board Abstraction

- **Purpose**: Hardware abstraction for the Waveshare ESP32-P4-WIFI6-Touch-LCD-4B (EP44B) board
- **Key files**: `p3a_board_display.c`, `p3a_board_fs.c`, `p3a_board_button.c`
- **Public API**: `p3a_board.h`
- **Key constants**:
  - `P3A_DISPLAY_WIDTH` (720), `P3A_DISPLAY_HEIGHT` (720)
  - `P3A_PIXEL_RGB888` / `P3A_PIXEL_RGB565`
  - `P3A_HAS_TOUCH`, `P3A_HAS_WIFI`, `P3A_HAS_USB`, `P3A_HAS_BUTTONS`
- **Key functions**: `p3a_board_display_init()`, `p3a_board_littlefs_mount()`, `p3a_board_littlefs_check_health()`, `p3a_board_webui_is_healthy()`, `p3a_board_set_brightness()`, `p3a_board_get_brightness()`, `p3a_board_sdcard_mount()`, `p3a_board_sdcard_unmount()`, `p3a_board_touch_init()`, `p3a_board_button_init()`
- **Kconfig**: `components/p3a_board_ep44b/Kconfig` — pixel format selection (`P3A_PIXEL_FORMAT_RGB888`)

## 5. channel_manager — Channel/Playlist Management

- **Purpose**: Manages animation channels, playlists, vault storage, and download coordination
- **Key interfaces**:
  - `channel_interface.h` — generic channel abstraction (`load`, `next_item`, `start_playback`)
  - `sdcard_channel_impl.h` — SD card file scanning and playback
  - `makapix_channel_impl.h` — Makapix Club channel implementation
  - `vault_storage.h` — SHA256-sharded artwork storage
  - `animation_metadata.h` — JSON sidecar metadata parsing
  - `download_manager.h` — download coordination
  - `playlist_manager.h` — playlist management
  - `channel_cache.h` — channel cache
  - `load_tracker.h` — load tracking
- **Key files**: `sdcard_channel.c`, `sdcard_channel_impl.c`, `makapix_channel_impl.c`, `makapix_channel_events.c`, `makapix_channel_refresh.c`, `makapix_channel_utils.c`, `vault_storage.c`, `animation_metadata.c`, `channel_cache.c`, `channel_metadata.c`, `channel_settings.c`, `download_manager.c`, `load_tracker.c`, `playlist_manager.c`
- **Kconfig**: `CHANNEL_MANAGER_PAGE_SIZE` (default 32)

## 6. wifi_manager — Connectivity

- **Purpose**: Wi-Fi STA/AP mode, captive portal, SNTP synchronization
- **Key files**: `app_wifi.c`, `sntp_sync.c`
- **Features**:
  - Auto-connect to saved credentials
  - Fallback to captive portal AP mode (`p3a-setup`)
  - mDNS `p3a.local` hostname
  - NTP time synchronization
- **Kconfig**: `WIFI_MANAGER_ENABLED`

## 7. animation_decoder — Image/Animation Decoders

- **Purpose**: Unified interface for image/animation decoding
- **Formats**: WebP (animated), GIF (animated), PNG, JPEG
- **Transparency**: Full alpha channel support for WebP, GIF, and PNG
- **Aspect ratio**: Preserves original aspect ratio when scaling non-square images
- **Key files**: `webp_animation_decoder.c`, `png_animation_decoder.c`, `jpeg_animation_decoder.c`
- **Public API** (`animation_decoder.h`):
  - `animation_decoder_init()` — open and initialize a decoder
  - `animation_decoder_get_info()` — get canvas size, frame count, transparency flag
  - `animation_decoder_decode_next()` — decode next frame to RGBA buffer
  - `animation_decoder_decode_next_rgb()` — decode next frame to RGB with alpha compositing
  - `animation_decoder_get_frame_delay()` — get frame duration in milliseconds
  - `animation_decoder_reset()` — reset to first frame
  - `animation_decoder_unload()` — free decoder resources
- **Kconfig**: `ANIMATION_DECODER_STATIC_FRAME_DELAY_MS` (default 100ms for static images)

## 8. animated_gif_decoder — GIF Decoder

- **Purpose**: GIF decoding via C++ wrapper around AnimatedGIF library
- **Key files**: `AnimatedGIF.cpp`, `gif_animation_decoder.cpp`, `gif.inl`

## 9. giphy — Giphy Integration

- **Purpose**: Fetch and cache trending GIFs from the Giphy API
- **Key files**: `giphy_api.c`, `giphy_cache.c`, `giphy_download.c`, `giphy_refresh.c`
- **Features**:
  - Fetches trending GIFs via the Giphy API (paginated, up to cache size)
  - SHA256-sharded file storage on SD card (`/sdcard/p3a/giphy/`)
  - Configurable rendition, format (WebP/GIF), content rating, refresh interval, and cache size
  - Periodic automatic refresh of trending content
  - On-demand download with atomic writes (temp file + rename)
- **Web UI**: Settings page at `/giphy`
- **Kconfig**: `GIPHY_API_KEY_DEFAULT`, `GIPHY_RENDITION_DEFAULT` (default `fixed_height`), `GIPHY_FORMAT_DEFAULT` (default `gif`)

## 10. makapix — Makapix Club Integration

- **Purpose**: Makapix Club MQTT integration for cloud-connected artwork sharing
- **Key files**: `makapix.c`, `makapix_mqtt.c`, `makapix_provision.c`, `makapix_provision_flow.c`, `makapix_store.c`, `makapix_api.c`, `makapix_artwork.c`, `makapix_certs.c`, `makapix_connection.c`, `makapix_channel_switch.c`, `makapix_refresh.c`, `makapix_single_artwork.c`, `view_tracker.c`
- **Features**:
  - Device provisioning via HTTPS (`makapix_provision.c`, `makapix_provision_flow.c`)
  - TLS MQTT with mTLS authentication (`makapix_mqtt.c`, `makapix_certs.c`)
  - Artwork receiving and playback (`makapix_artwork.c`, `makapix_single_artwork.c`)
  - Remote command receiving (swap_next, swap_back, etc.)
  - Channel switching and refresh (`makapix_channel_switch.c`, `makapix_refresh.c`)
  - View tracking analytics (`view_tracker.c`)
  - NVS credential storage (`makapix_store.c`)
- **Kconfig**: `components/makapix/Kconfig`

## 11. http_api — HTTP Server and REST API

- **Purpose**: HTTP server, REST API, WebSocket handler, static file serving
- **Key files**: `http_api.c`, `http_api_rest_status.c`, `http_api_rest_actions.c`, `http_api_rest_settings.c`, `http_api_rest_playsets.c`, `http_api_ota.c`, `http_api_upload.c`, `http_api_pages.c`, `http_api_pico8.c`, `http_api_utils.c`
- **Features**: mDNS, static file serving from LittleFS, file upload, playset CRUD, OTA endpoints
- See [Network and API](network-and-api.md) for the full endpoint list.

## 12. config_store — NVS-Backed Configuration

- **Purpose**: Persistent configuration with NVS backend
- **Key files**: `config_store.c`, `config_store.h`
- **Stores**: Wi-Fi credentials, brightness, auto-swap interval, rotation, background color, Giphy settings, play order, dwell time, global seed

## 13. ota_manager — Over-the-Air Updates

- **Purpose**: Wireless firmware and web UI updates from GitHub Releases
- **Key files**: `ota_manager.c`, `ota_manager_install.c`, `ota_manager_webui.c`, `github_ota.c`
- **Features**:
  - Automatic periodic update checks (every 2 hours)
  - Separate firmware and web UI update paths
  - Web UI for manual check, install, and rollback
  - SHA256 checksum verification
  - Progress display on LCD during updates
  - Automatic rollback if firmware fails to boot 3 times
- **Kconfig**: `components/ota_manager/Kconfig`

## 14. slave_ota — ESP32-C6 Co-processor Firmware

- **Purpose**: Automatic firmware management for the ESP32-C6 Wi-Fi co-processor
- **Key files**: `slave_ota.c`, `slave_ota.h`
- **Features**:
  - Detects outdated or missing co-processor firmware
  - Automatically flashes ESP-Hosted firmware during boot
  - Progress display on LCD during flashing

## 15. pico8 — PICO-8 Streaming

- **Purpose**: PICO-8 game streaming over WebSocket
- **Key files**: `pico8_stream.c`, `pico8_render.c`, `pico8_stream_stubs.c`
- **Features**:
  - 128x128 indexed pixel frames with 16-color palette
  - Nearest-neighbor upscaling to 720x720
  - Auto-timeout after 30 seconds
- **Kconfig**: `P3A_PICO8_ENABLE`, `P3A_PICO8_USB_STREAM_ENABLE`

## 16. content_cache — Channel Cache Wrapper

- **Purpose**: Thin wrapper around `download_manager` for legacy compatibility
- **Key files**: `content_cache.c`
- **Public API**: `content_cache.h`
- **Key functions**: `content_cache_init()`, `content_cache_deinit()`, `content_cache_is_busy()`, `content_cache_set_channels()`, `content_cache_reset_cursors()`, `content_cache_rescan()`

## 17. content_source — Channel Content Source Abstraction

- **Purpose**: Simple wrapper around `channel_handle_t` for content retrieval
- **Key files**: `content_source.c`
- **Public API**: `content_source.h`
- **Key functions**: `content_source_init_from_channel()`, `content_source_refresh()`, `content_source_get_post()`, `content_source_get_count()`

## 18. loader_service — Animation File Loader

- **Purpose**: Loads animation files from SD card into memory and initializes decoders
- **Key files**: `loader_service.c`
- **Public API**: `loader_service.h`
- **Key functions**: `loader_service_load()`, `loader_service_unload()`
- **Features**: Chunked SD card reading with retries, PSRAM allocation preference, yields between chunks

## 19. playback_queue — Play Scheduler to Animation Player Adapter

- **Purpose**: Converts `ps_artwork_t` (play scheduler output) to `swap_request_t` (animation player input)
- **Key files**: `playback_queue.c`
- **Public API**: `playback_queue.h`
- **Key functions**: `playback_queue_current()`, `playback_queue_next()`, `playback_queue_prev()`, `playback_queue_peek()`

## 20. sdio_bus — SDIO Bus Coordinator

- **Purpose**: Mutex-based coordination for shared SDIO bus between WiFi (SDIO Slot 1) and SD card (SDMMC Slot 0)
- **Key files**: `sdio_bus.c`
- **Public API**: `sdio_bus.h`
- **Key functions**: `sdio_bus_init()`, `sdio_bus_acquire()`, `sdio_bus_release()`, `sdio_bus_is_locked()`, `sdio_bus_get_holder()`
- **Use case**: Prevents "SDIO slave unresponsive" crashes during OTA or large downloads

## 21. show_url — URL Artwork Downloader

- **Purpose**: Downloads artwork from arbitrary HTTP/HTTPS URLs and plays them
- **Key files**: `show_url.c`
- **Public API**: `show_url.h`
- **Key functions**: `show_url_init()`, `show_url_start()`, `show_url_cancel()`, `show_url_is_busy()`
- **Features**:
  - Format detection from magic bytes (PNG, JPEG, GIF, WebP)
  - Unique filename generation
  - Chunked download (128 KB chunks, 16 MiB max)
  - Auto-refreshes SD card cache and starts playback

## 22. debug_http_log — Performance Instrumentation

- **Purpose**: Compile-time optional performance statistics for frame rendering
- **Key files**: `debug_http_log.c`, `debug_http_log.h`
- **Key functions** (only when `CONFIG_P3A_PERF_DEBUG=1`): `debug_perf_record_frame()`, `debug_perf_record_decode_detail()`, `debug_perf_flush_stats()`
- **Statistics**: decode time, upscale time, total render time, late frames, alpha usage
- When disabled, all functions are no-ops with zero overhead.

## 23. app_state — Application State (Legacy)

- **Purpose**: Simple centralized application state (ready, processing, error)
- **Note**: The primary state management is now handled by `p3a_core`. This component remains for backward compatibility.

## 24. Supporting Libraries

| Component | Purpose |
|-----------|---------|
| `ugfx` | uGFX graphics library for UI rendering |
| `libwebp_decoder` | libwebp wrapper for WebP decoding |
