# Component Architecture

All 25 custom components live under `components/`. This document describes each one.

---

## 1. p3a_core — Unified State Machine and Lifecycle

- **Purpose**: Central state machine, touch routing, rendering dispatch, SD path management, and boot utilities
- **Key files**: `p3a_state.c`, `p3a_state_channel.c`, `p3a_state_connectivity.c`, `p3a_render.c`, `p3a_touch_router.c`, `p3a_current_post.c`, `sd_path.c`, `p3a_boot_logo.c`, `p3a_logo.c`, `fresh_boot.c`
- **Public API**: `p3a_state.h`, `p3a_render.h`, `p3a_touch_router.h`, `p3a_current_post.h`, `p3a_logo.h`, `p3a_boot_logo.h`, `p3a_limits.h`, `sd_path.h`, `fresh_boot.h`
- **States**: `BOOT`, `ANIMATION_PLAYBACK`, `PROVISIONING`, `OTA`, `PICO8_STREAMING`, `ERROR`
- **Key functions**:
  - `p3a_state_init()`, `p3a_state_get()`, `p3a_state_enter_*()`, `p3a_state_fallback_to_sdcard()` — state transitions
  - `p3a_touch_router_init()`, `p3a_touch_router_handle_event()` — state-aware touch routing
  - `p3a_render_init()`, `p3a_render_frame()` — state-aware rendering dispatch
  - `sd_path_init()`, `sd_path_get_*()` — configurable SD card root directory (default `/sdcard/p3a`)

## 2. play_scheduler — Deterministic Multi-Channel Playback Engine

- **Purpose**: Executes playsets to select artwork across multiple channels using Smooth Weighted Round Robin (SWRR)
- **Key files**: `play_scheduler.c`, `play_scheduler_swrr.c`, `play_scheduler_playsets.c`, `play_scheduler_pick.c`, `play_scheduler_navigation.c`, `play_scheduler_timer.c`, `play_scheduler_nae.c`, `play_scheduler_lai.c`, `play_scheduler_refresh.c`, `play_scheduler_buffers.c`, `play_scheduler_cache.c`, `playset_store.c`, `playset_json.c`, `active_playset_store.c`
- **Public API**: `play_scheduler.h`, `play_scheduler_types.h`, `playset_store.h`, `playset_json.h`, `active_playset_store.h`
- **Boot restore**: Every `play_scheduler_execute_playset()` writes `{sd-root}/active_playset.bin` (root resolved at runtime via `sd_path_get_root()`, default `/sdcard/p3a`; full `ps_playset_t` blob, magic+version+CRC, atomic rename). On boot, `animation_player_restore_boot_playset()` loads that snapshot and re-executes it; on any failure falls back to channel_promoted. Because the snapshot lives under the configurable root, switching the root cold-starts the playset too.
- **Key concepts**:
  - **Playset** (`ps_playset_t`): Specifies which channels to include (with per-channel weights) and pick mode
  - **Channel weights**: Each channel has a `weight`. SWRR distributes plays proportionally; if every weight is 0, plays are distributed equally.
  - **Pick modes**: Recency-based, Random
  - **NAE**: New Artwork Events — react to freshly downloaded content
  - **LAi**: Locally Available index — persistence of playback position per channel
- **Key functions**: `play_scheduler_init()`, `play_scheduler_execute_playset()`, `play_scheduler_next()`, `play_scheduler_prev()`, `play_scheduler_peek_next()`, `play_scheduler_play_named_channel()`, `play_scheduler_play_artwork()`
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
  - `channel_interface.h` — generic channel abstraction (`load`, `unload`, `start_playback`, `request_reshuffle`, `request_refresh`, `get_stats`, `get_post_count`, `get_post`, `get_navigator`, `destroy`)
  - `sdcard_channel_impl.h` — SD card file scanning and playback
  - `makapix_channel_impl.h` — Makapix Club channel implementation
  - `download_manager.h` — download coordination
  - `playlist_manager.h` — playlist management
  - `channel_cache.h` — channel cache
- **Key files**: `sdcard_channel.c`, `sdcard_channel_impl.c`, `makapix_channel_impl.c`, `makapix_channel_events.c`, `makapix_channel_refresh.c`, `makapix_channel_utils.c`, `channel_cache.c`, `channel_cache_ops.c`, `channel_cache_merge.c`, `channel_metadata.c`, `channel_settings.c`, `download_manager.c`, `playlist_manager.c`
- **Vault sharding**: 2-level shard (`SD_SHARD_DEPTH`) of 6 bits each (decimal dirs `0`–`63`), derived from the first two bytes of the FNV-1a-64+fmix64 hash of the sanitized leaf filename, e.g. `/sdcard/p3a/vault/{d0}/{d1}/<storage_key>.<ext>`. All vault/giphy/museum local path-building is centralized in `sd_path_build_sharded()` (`p3a_core/sd_path.c`), with the Makapix family wrapper `makapix_build_vault_path()` in `makapix_channel_utils.c`. The Makapix server's vault URL shard (`https://vault.makapix.club/{aa}/{bb}/...`, `CONFIG_MAKAPIX_VAULT_HOST`) is a **separate**, SHA256-based constant — `MAKAPIX_REMOTE_SHARD_DEPTH` + `MAKAPIX_REMOTE_SHARD_MASK` (`makapix_build_remote_url()`) — because it mirrors a server contract, not the local disk layout.
- **Kconfig**: `CHANNEL_MANAGER_PAGE_SIZE` (default 32)

## 6. wifi_manager — Connectivity

- **Purpose**: Wi-Fi STA/AP mode, captive portal, recovery, SNTP synchronization
- **Key files**: `app_wifi.c`, `wifi_captive_portal.c`, `wifi_recovery.c`, `sntp_sync.c`
- **Features**:
  - Auto-connect to saved credentials
  - Fallback to captive portal AP mode (`p3a-setup`)
  - mDNS `p3a.local` hostname
  - NTP time synchronization
- **Kconfig**: `WIFI_MANAGER_ENABLED`

## 7. animation_decoder — Image/Animation Decoders

- **Purpose**: Unified interface for image/animation decoding
- **Formats**: WebP (animated), GIF (animated), PNG/APNG (static/animated), JPEG, BMP
- **Transparency**: Full alpha channel support for WebP, GIF, and PNG/APNG; BMP when the file carries an explicit alpha mask (V4/V5 headers)
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
  - Hash-sharded file storage on SD card (`/sdcard/p3a/giphy/`)
  - Configurable rendition, format (WebP/GIF), content rating, refresh interval, and cache size
  - Periodic automatic refresh of trending content
  - On-demand download with atomic writes (temp file + rename)
- **Web UI**: Settings page at `/giphy`
- **Kconfig**: `GIPHY_API_KEY_DEFAULT`, `GIPHY_RENDITION_DEFAULT` (default `fixed_height`), `GIPHY_FORMAT_DEFAULT` (default `gif`)

## 10. art_institution — Museum (IIIF) Channels

- **Purpose**: First-class channel source for artwork hosted by major museums that expose their collections through the [IIIF Image API](https://iiif.io/api/image/3.0/). Seven museums ship today: the Art Institute of Chicago (`artic`), the Rijksmuseum (`rijks`), the Victoria and Albert Museum (`vam`), the Wellcome Collection (`wellcome`), the Statens Museum for Kunst (`smk`), Harvard Art Museums (`ham`, BYOK API key), and the Smithsonian (`si`, BYOK api.data.gov key).
- **Key files**: `art_institution.c` (dispatch + lifecycle), `art_institution_refresh.c` (per-channel listing walk), `art_institution_download.c` (IIIF JPEG fetch with NRS→IDS redirect shim), `art_institution_resolve.c` (Rijks Linked-Art walk), `art_institution_rate_limit.c` (per-museum cooldown), `museums/{artic,rijksmuseum,vam,wellcome,smk,ham,smithsonian}.c` (per-museum adapters), `museums/common.c` (shared HTTP helpers)
- **Public API**: `art_institution.h`, `art_institution_types.h`
- **Architecture**:
  - **Browser side** owns browse (museum → axis → term selection); each museum has a matching JS adapter under `webui/museum/`. The browser talks directly to museum APIs over CORS.
  - **Device side** owns refresh, IIIF download, and playback. Each museum exposes the same C dispatch shape: `refresh_channel`, `build_iiif_url`, and optional `resolve_entry` (for museums like Rijks that require a multi-hop Linked-Art walk to discover the image id).
- **Storage**: Cached images live at `/sdcard/p3a/museum/{museum_id}/{d0}/{d1}/{iiif_key}.{ext}` (6-bit decimal shard dirs — see `sd_path_build_sharded()`). The vault is shared across all channels of the same museum, so an artwork that belongs to several facets is only stored once.
- **Rate limiting**: A per-museum cooldown table is shared between the device's refresh dispatcher, the download manager, and the browser's browse modal. Browser-issued 429s are reported back to the device via `POST /api/museum/rate-limits/report-429` so the per-IP budget stays consistent. The browse modal reads `GET /api/museum/rate-limits` before triggering term-count probes.
- **Settings**: Two global NVS keys, `ai_refresh_sec` (default 345 600 s — 4 days) and `ai_cache_size` (default 1024 entries per channel), surface in the **Museum** tab of the settings page. Per-museum BYOK keys live in the same tab: `ham_api_key` (Harvard Art Museums) and `si_api_key` (Smithsonian, via api.data.gov). For both: no key is shipped, refresh is a no-op until the user enters one, and clearing the key dormants the refresh path without dropping the channel.
- **Storage eviction**: Museum vault files are reclaimed by the existing `storage_eviction` component alongside the Makapix vault and Giphy cache (see component 17).
- **Playset binary format**: Institution channels use `PS_CHANNEL_TYPE_INSTITUTION = 7` with `name = "{museum_id}:{axis}"` and `identifier = "{term_id}"`. Cache entries use `institution_channel_entry_t` (64 bytes; same slot as the makapix/giphy entries) with the discriminator `PS_ENTRY_FORMAT_INSTITUTION`.
- **Full design**: See [`docs/art-institutions/finalized-design.md`](../art-institutions/finalized-design.md).

## 11. makapix — Makapix Club Integration

- **Purpose**: Makapix Club MQTT integration for cloud-connected artwork sharing
- **Key files**: `makapix.c`, `makapix_mqtt.c`, `makapix_provision.c`, `makapix_provision_flow.c`, `makapix_store.c`, `makapix_api.c`, `makapix_artwork.c`, `makapix_certs.c`, `makapix_connection.c`, `makapix_channel_switch.c`, `makapix_promoted_https.c`, `makapix_refresh.c`, `makapix_single_artwork.c`, `view_tracker.c`
- **Features**:
  - Device provisioning via HTTPS (`makapix_provision.c`, `makapix_provision_flow.c`)
  - TLS MQTT with mTLS authentication (`makapix_mqtt.c`, `makapix_certs.c`)
  - Artwork receiving and playback (`makapix_artwork.c`, `makapix_single_artwork.c`)
  - Remote command receiving — exactly: `swap_next`, `swap_back`, `set_background_color`, `play_channel`, `show_artwork`, `show_url`, `swap_to`, `execute_playset`
  - Channel switching and refresh (`makapix_channel_switch.c`, `makapix_refresh.c`)
  - View tracking analytics (`view_tracker.c`)
  - NVS credential storage (`makapix_store.c`)
- **Kconfig**: `components/makapix/Kconfig`

## 12. http_api — HTTP Server and REST API

- **Purpose**: HTTP server, REST API, WebSocket handler, static file serving
- **Key files**: `http_api.c`, `http_api_rest_status.c`, `http_api_rest_actions.c`, `http_api_rest_settings.c`, `http_api_rest_playsets.c`, `http_api_ota.c`, `http_api_upload.c`, `http_api_pages.c`, `http_api_pico8.c`, `http_api_utils.c`
- **Features**: mDNS, static file serving from LittleFS, file upload, playset CRUD, OTA endpoints
- See [Network and API](network-and-api.md) for the full endpoint list.

## 13. config_store — NVS-Backed Configuration

- **Purpose**: Persistent configuration with NVS backend
- **Key files**: `config_store.c`, `config_store_settings.c`, `config_store_giphy.c`, plus internal headers
- **Public API**: `config_store.h`
- **Stores**: rotation, background color, dwell time, refresh interval, randomize playlist, show FPS, max-speed playback, view-ack, SD card root, channel-cache size, processing-notification options, channel-selection mode, Giphy fields (api_key, rendition, format, rating, country code, random_id, cache size, refresh interval, prefer-downsized), device name and hostname, Wi-Fi/touch recovery counters
- **Stored elsewhere**: Wi-Fi credentials live in NVS namespaces handled by `wifi_manager`; brightness is owned by `p3a_board`; Makapix credentials live in `makapix_store`; playsets are persisted by `playset_store`.

## 14. ota_manager — Over-the-Air Updates

- **Purpose**: Wireless firmware and web UI updates from GitHub Releases
- **Key files**: `ota_manager.c`, `ota_manager_install.c`, `ota_manager_webui.c`, `github_ota.c`
- **Features**:
  - Automatic periodic update checks (default every 12 hours, configurable via `CONFIG_OTA_CHECK_INTERVAL_HOURS`)
  - Updates are never installed automatically — installation is gated on a user POST to `/ota/install`
  - Separate firmware and web UI update paths
  - Web UI for manual check, install, and rollback (rollback button label is "Rollback to <previous-version>")
  - SHA256 checksum verification of every download
  - Progress display on both LCD and web UI during updates
- **Kconfig**: `components/ota_manager/Kconfig`

## 15. slave_ota — ESP32-C6 Co-processor Firmware

- **Purpose**: Automatic firmware management for the ESP32-C6 Wi-Fi co-processor
- **Key files**: `slave_ota.c`, `slave_ota.h`
- **Features**:
  - Detects outdated or missing co-processor firmware
  - Automatically flashes ESP-Hosted firmware during boot
  - Progress logged to the serial console only; the LCD keeps showing whatever the animation player was rendering, then blanks at the post-update `esp_restart()`

## 16. pico8 — PICO-8 Streaming

- **Purpose**: PICO-8 game streaming over WebSocket (with optional audio)
- **Key files**: `pico8_stream.c`, `pico8_render.c`, `pico8_audio.c`, `pico8_stream_stubs.c`
- **Features**:
  - 128x128 indexed pixel frames with 16-color palette
  - Nearest-neighbor upscaling to 720x720
  - Audio streaming (when `CONFIG_P3A_PICO8_AUDIO_ENABLE`)
  - Auto-timeout after 30 seconds of inactivity
- **Kconfig**: `P3A_PICO8_ENABLE`, `P3A_PICO8_USB_STREAM_ENABLE`, `P3A_PICO8_AUDIO_ENABLE`

## 17. content_cache — Channel Cache Wrapper

- **Purpose**: Thin wrapper around `download_manager` for legacy compatibility
- **Key files**: `content_cache.c`
- **Public API**: `content_cache.h`
- **Key functions**: `content_cache_init()`, `content_cache_deinit()`, `content_cache_is_busy()`, `content_cache_set_channels()`, `content_cache_reset_cursors()`, `content_cache_rescan()`

## 18. storage_eviction — SD Card Space Management

- **Purpose**: Age-based eviction of cached artwork and stale channel files from SD card. The cache walk is layout-unaware: it recurses into whatever directories exist under the vault/giphy/museum bases (depth capped for stack safety, not layout), deletes by extension allowlist + age — artwork files plus `.404` negative-cache markers and orphaned `.tmp` staging files — and `rmdir`s emptied directories on the way out (this is also what gradually reclaims orphaned pre-1.0 SHA256 shard trees). File mtime is touched on every successful load by the animation player, so age ≈ time since last played.
- **Key files**: `storage_eviction.c`
- **Public API**: `storage_eviction.h`
- **Key functions**:
  - `storage_eviction_check_and_run()` — multi-pass age-based eviction of the vault/giphy/museum cache trees
  - `storage_eviction_get_free_space()` — query free bytes on /sdcard
  - `storage_eviction_get_storage_info()` — query total and free bytes
  - `channel_eviction_check_and_run()` — evict stale channel metadata files
- **Policy**: Two-watermark hysteresis. Triggers when free space drops below `STORAGE_EVICTION_TARGET_MIB`, then evicts until free space reaches `STORAGE_EVICTION_TARGET_MIB + STORAGE_EVICTION_HEADROOM_MIB`. With defaults (1024 + 4096 = 5120 MiB stop watermark), p3a keeps **at least ~5 GiB free** at all times. Cards smaller than this can't satisfy the stop watermark and would trigger eviction churn — **8 GB minimum recommended for SD card sizing**.
- **Small-card guard**: If the SD card's total capacity is below `STORAGE_EVICTION_MIN_CARD_SIZE_MIB` (default 7000 MiB, sized below the formatted capacity of an advertised 8 GB card), eviction skips silently. The device fills the card and stops accepting new downloads rather than thrashing the FS. Set the Kconfig to 0 to disable the guard.
- **Kconfig**: `STORAGE_EVICTION_TARGET_MIB` (trigger watermark, default 1024 MiB), `STORAGE_EVICTION_HEADROOM_MIB` (overshoot above trigger, default 4096 MiB), `STORAGE_EVICTION_MIN_CARD_SIZE_MIB` (small-card guard, default 7000 MiB), `STORAGE_EVICTION_INITIAL_AGE_DAYS` (default 30), `STORAGE_EVICTION_MIN_AGE_HOURS` (default 4), `CHANNEL_EVICTION_AGE_DAYS` (default 60)

## 19. loader_service — Animation File Loader

- **Purpose**: Loads animation files from SD card into memory and initializes decoders
- **Key files**: `loader_service.c`
- **Public API**: `loader_service.h`
- **Key functions**: `loader_service_load()`, `loader_service_unload()`
- **Features**: Chunked SD card reading with retries, PSRAM allocation preference, yields between chunks

## 20. playback_queue — Play Scheduler to Animation Player Adapter

- **Purpose**: Converts `ps_artwork_t` (play scheduler output) to `swap_request_t` (animation player input)
- **Key files**: `playback_queue.c`
- **Public API**: `playback_queue.h`
- **Key functions**: `playback_queue_current()`, `playback_queue_next()`, `playback_queue_prev()`, `playback_queue_peek()`

## 21. sdio_bus — SDIO Bus Coordinator

- **Purpose**: Mutex-based coordination for shared SDIO bus between WiFi (SDIO Slot 1) and SD card (SDMMC Slot 0)
- **Key files**: `sdio_bus.c`
- **Public API**: `sdio_bus.h`
- **Key functions**: `sdio_bus_init()`, `sdio_bus_acquire()`, `sdio_bus_release()`, `sdio_bus_is_locked()`, `sdio_bus_get_holder()`
- **Use case**: Prevents "SDIO slave unresponsive" crashes during OTA or large downloads

## 22. show_url — URL Artwork Downloader

- **Purpose**: Downloads artwork from arbitrary HTTP/HTTPS URLs and plays them
- **Key files**: `show_url.c`
- **Public API**: `show_url.h`
- **Key functions**: `show_url_init()`, `show_url_start()`, `show_url_cancel()`, `show_url_is_busy()`
- **Features**:
  - Format detection from magic bytes (PNG, JPEG, GIF, WebP)
  - Unique filename generation
  - Chunked download (128 KB chunks, 16 MiB max)
  - Auto-refreshes SD card cache and starts playback

## 23. debug_http_log — Performance Instrumentation

- **Purpose**: Compile-time optional performance statistics for frame rendering
- **Key files**: `debug_http_log.c`, `debug_http_log.h`
- **Key functions** (only when `CONFIG_P3A_PERF_DEBUG=1`): `debug_perf_record_frame()`, `debug_perf_record_decode_detail()`, `debug_perf_flush_stats()`
- **Statistics**: decode time, upscale time, total render time, late frames, alpha usage
- When disabled, all functions are no-ops with zero overhead.

## 24. Supporting Libraries

| Component | Purpose |
|-----------|---------|
| `ugfx` | uGFX text/font rendering subset (DejaVu Sans 16/24/32 only; no graphics primitives enabled) |
| `libwebp_decoder` | libwebp wrapper for WebP decoding |
