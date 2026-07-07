# Directory Structure

```
p3a/
├── main/                                # Main application component
│   ├── p3a_main.c                       # Application entry point
│   ├── app_lcd_p4.c                     # LCD initialization and control
│   ├── app_touch.c                      # Touch input handling
│   ├── app_usb.c                        # USB composite device (conditional: P3A_USB_MSC_ENABLE)
│   ├── usb_descriptors.c                # USB device descriptors (conditional: P3A_USB_MSC_ENABLE)
│   ├── usb_descriptors.h                # USB descriptor definitions
│   ├── tusb_config.h                    # TinyUSB configuration
│   ├── display_renderer.c               # Frame buffer management, vsync
│   ├── display_renderer_priv.h          # Display renderer private definitions
│   ├── display_upscaler.c               # Parallel CPU nearest-neighbor upscaling
│   ├── display_ppa_upscaler.c           # PPA hardware bilinear upscaling (conditional: P3A_PPA_UPSCALE_ENABLE)
│   ├── display_fps_overlay.c            # FPS counter overlay
│   ├── display_processing_notification.c  # Swap processing/failure indicator
│   ├── display_reaction_overlay.c       # Reaction submit/revoke overlay
│   ├── render_engine.c                  # Display rotation and background color API
│   ├── animation_player.c               # Core animation engine
│   ├── animation_player_render.c        # Frame rendering (decode + upscale routing)
│   ├── animation_player_loader.c        # Asset loading and aspect-ratio map building
│   ├── animation_player_priv.h          # Animation player private definitions
│   ├── playback_controller.c            # Source switching (animation, PICO-8)
│   ├── connectivity_service.c           # Wi-Fi and OTA service wrapper
│   ├── content_service.c                # Content cache service wrapper
│   ├── playback_service.c               # Play scheduler service wrapper
│   ├── ugfx_ui.c                        # uGFX-based on-screen text/UI rendering
│   ├── reaction_submit_img.c/.h         # Embedded thumbs-up overlay image
│   ├── reaction_revoke_img.c/.h         # Embedded revoke overlay image
│   ├── reaction_error_img.c/.h          # Embedded error overlay image
│   ├── submit_click_img.c/.h            # Embedded Giphy click overlay image
│   ├── CMakeLists.txt                   # Main component build config
│   ├── Kconfig.projbuild                # Project-level Kconfig menu
│   ├── idf_component.yml                # ESP Component Registry manifest
│   ├── component.mk                     # Legacy make compatibility
│   └── include/                         # Public headers
│       ├── animation_player.h
│       ├── app_lcd.h
│       ├── app_touch.h
│       ├── app_usb.h
│       ├── connectivity_service.h
│       ├── content_service.h
│       ├── display_ppa_upscaler.h
│       ├── display_renderer.h
│       ├── playback_controller.h
│       ├── playback_service.h
│       ├── render_engine.h
│       ├── ugfx_ui.h
│       └── version.h
│
├── components/                          # Custom components (25 total)
│   ├── p3a_core/                        # Unified state machine and lifecycle
│   │   ├── p3a_state.c                  # Global state machine
│   │   ├── p3a_state_channel.c          # Channel-related state transitions
│   │   ├── p3a_state_connectivity.c     # Connectivity-related state transitions
│   │   ├── p3a_state_internal.h
│   │   ├── p3a_render.c                 # State-aware rendering dispatch
│   │   ├── p3a_touch_router.c           # Touch event routing by state
│   │   ├── p3a_current_post.c           # Current post tracking
│   │   ├── p3a_logo.c                   # Logo blitting utilities
│   │   ├── p3a_boot_logo.c              # Boot logo with fade-in
│   │   ├── sd_path.c                    # Configurable SD card root path
│   │   ├── fresh_boot.c                 # Debug NVS/SD erase utilities
│   │   ├── include/
│   │   │   ├── p3a_state.h
│   │   │   ├── p3a_render.h
│   │   │   ├── p3a_touch_router.h
│   │   │   ├── p3a_current_post.h
│   │   │   ├── p3a_logo.h
│   │   │   ├── p3a_boot_logo.h
│   │   │   ├── p3a_limits.h
│   │   │   ├── sd_path.h
│   │   │   └── fresh_boot.h
│   │   └── CMakeLists.txt
│   │
│   ├── play_scheduler/                  # Deterministic multi-channel playback engine
│   │   ├── play_scheduler.c             # Core scheduler logic
│   │   ├── play_scheduler_swrr.c        # Smooth Weighted Round Robin
│   │   ├── play_scheduler_playsets.c    # Playset execution
│   │   ├── play_scheduler_pick.c        # Artwork picking
│   │   ├── play_scheduler_navigation.c  # Next/prev navigation
│   │   ├── play_scheduler_timer.c       # Dwell time timer
│   │   ├── play_scheduler_lai.c         # Locally Available index tracking
│   │   ├── play_scheduler_refresh.c     # Channel refresh
│   │   ├── play_scheduler_buffers.c     # Buffer management
│   │   ├── play_scheduler_cache.c       # Cache integration
│   │   ├── playset_store.c              # Playset persistence (NVS)
│   │   ├── playset_json.c               # Playset JSON serialization
│   │   ├── include/
│   │   │   ├── play_scheduler.h
│   │   │   ├── play_scheduler_internal.h
│   │   │   ├── play_scheduler_types.h
│   │   │   ├── playset_store.h
│   │   │   └── playset_json.h
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   │
│   ├── event_bus/                       # Asynchronous event pub/sub
│   │   ├── event_bus.c
│   │   ├── include/event_bus.h
│   │   └── CMakeLists.txt
│   │
│   ├── p3a_board_ep44b/                 # Board abstraction (EP44B hardware)
│   │   ├── p3a_board_display.c          # Display hardware init and brightness
│   │   ├── p3a_board_fs.c               # LittleFS and SD card mount
│   │   ├── p3a_board_button.c           # BOOT button support
│   │   ├── include/p3a_board.h          # Public board API
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   │
│   ├── channel_manager/                 # Channel/playlist management (vault path logic lives here too)
│   │   ├── sdcard_channel.c             # SD card channel
│   │   ├── sdcard_channel_impl.c        # SD card implementation
│   │   ├── makapix_channel_impl.c       # Makapix channel implementation
│   │   ├── makapix_channel_events.c     # Makapix event handling
│   │   ├── makapix_channel_refresh.c    # Makapix channel refresh
│   │   ├── makapix_channel_utils.c      # Makapix channel utilities
│   │   ├── makapix_channel_internal.h
│   │   ├── channel_cache.c              # Channel cache
│   │   ├── channel_cache_ops.c          # Vault path / per-asset cache ops (2-level hash sharding)
│   │   ├── channel_cache_merge.c        # Cache merge logic
│   │   ├── channel_cache_internal.h
│   │   ├── channel_metadata.c           # Per-channel metadata
│   │   ├── channel_settings.c           # Channel settings
│   │   ├── download_manager.c           # Download coordination
│   │   ├── playlist_manager.c           # Playlist management (incl. vault path helpers)
│   │   ├── pcg32_reversible.h
│   │   ├── include/
│   │   │   ├── channel_interface.h
│   │   │   ├── sdcard_channel.h
│   │   │   ├── sdcard_channel_impl.h
│   │   │   ├── makapix_channel_impl.h
│   │   │   ├── makapix_channel_events.h
│   │   │   ├── makapix_channel_utils.h
│   │   │   ├── channel_cache.h
│   │   │   ├── channel_metadata.h
│   │   │   ├── channel_settings.h
│   │   │   ├── download_manager.h
│   │   │   ├── playlist_manager.h
│   │   │   ├── animation_swap_request.h
│   │   │   ├── psram_alloc.h
│   │   │   ├── pcg32_reversible.h
│   │   │   └── uthash.h
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   │
│   ├── wifi_manager/                    # Wi-Fi, captive portal, recovery, SNTP
│   │   ├── app_wifi.c                   # Wi-Fi STA/AP, mDNS
│   │   ├── wifi_captive_portal.c        # Captive portal AP, DNS hijack, /save form
│   │   ├── wifi_recovery.c              # Reconnection / WPA3 fallback
│   │   ├── wifi_manager_internal.h
│   │   ├── sntp_sync.c                  # NTP time synchronization
│   │   ├── include/
│   │   │   ├── app_wifi.h
│   │   │   └── sntp_sync.h
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   │
│   ├── animation_decoder/               # Image/animation decoders
│   │   ├── webp_animation_decoder.c     # Animated WebP via libwebp
│   │   ├── png_animation_decoder.c      # PNG (single-frame) via libpng
│   │   ├── jpeg_animation_decoder.c     # JPEG via ESP-IDF hardware JPEG
│   │   ├── static_image_decoder_common.h
│   │   ├── include/
│   │   │   ├── animation_decoder.h
│   │   │   └── animation_decoder_internal.h
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   │
│   ├── animated_gif_decoder/            # GIF decoder (C++ wrapper around AnimatedGIF)
│   │   ├── AnimatedGIF.cpp
│   │   ├── gif_animation_decoder.cpp
│   │   ├── gif.inl
│   │   ├── include/
│   │   │   ├── AnimatedGIF.h
│   │   │   └── arduino_compat.h
│   │   └── CMakeLists.txt
│   │
│   ├── giphy/                           # Giphy API integration
│   │   ├── giphy_api.c                  # API calls
│   │   ├── giphy_cache.c                # SD card cache (2-level hash-sharded)
│   │   ├── giphy_download.c             # Download with atomic writes
│   │   ├── giphy_refresh.c              # Periodic refresh
│   │   ├── include/
│   │   │   ├── giphy.h
│   │   │   └── giphy_types.h
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   │
│   ├── klipy/                           # Klipy API integration (GIFs + stickers)
│   │   ├── klipy_api.c                  # API calls (trending/search/category, id re-resolve)
│   │   ├── klipy_cache.c                # SD card cache (2-level hash-sharded, gif|sticker split)
│   │   ├── klipy_download.c             # Download with atomic writes
│   │   ├── klipy_refresh.c              # Periodic refresh
│   │   ├── klipy_internal.h
│   │   ├── include/
│   │   │   ├── klipy.h
│   │   │   └── klipy_types.h
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   │
│   ├── art_institution/                 # Museum (IIIF) channel source
│   │   ├── art_institution.c            # Public API, dispatch table, lifecycle
│   │   ├── art_institution_refresh.c    # Per-channel refresh dispatcher
│   │   ├── art_institution_download.c   # IIIF JPEG download (HTTPS, atomic writes)
│   │   ├── art_institution_resolve.c    # Lazy resolver loop (e.g. Rijks Linked-Art walk)
│   │   ├── art_institution_rate_limit.c # Per-museum cooldown table
│   │   ├── art_institution_internal.h
│   │   ├── museums/
│   │   │   ├── common.c                 # Shared HTTP / parse helpers
│   │   │   ├── artic.c                  # Art Institute of Chicago adapter
│   │   │   ├── rijksmuseum.c            # Rijksmuseum adapter (3-hop Linked-Art walk)
│   │   │   ├── vam.c                    # Victoria and Albert Museum adapter
│   │   │   ├── wellcome.c               # Wellcome Collection adapter
│   │   │   └── smk.c                    # Statens Museum for Kunst adapter
│   │   ├── include/
│   │   │   ├── art_institution.h
│   │   │   └── art_institution_types.h  # institution_channel_entry_t, museum_id_t
│   │   └── CMakeLists.txt
│   │
│   ├── makapix/                         # Makapix Club integration
│   │   ├── makapix.c                    # Module init and lifecycle
│   │   ├── makapix_mqtt.c               # MQTT client (mTLS over TLS 1.2)
│   │   ├── makapix_provision.c          # Device provisioning
│   │   ├── makapix_provision_flow.c     # Provisioning state machine
│   │   ├── makapix_store.c              # NVS credential storage
│   │   ├── makapix_api.c                # HTTPS API calls
│   │   ├── makapix_artwork.c            # Artwork receiving (vault writer)
│   │   ├── makapix_certs.c              # TLS certificate handling
│   │   ├── makapix_connection.c         # Connection management
│   │   ├── makapix_channel_switch.c     # Channel switching
│   │   ├── makapix_promoted_https.c     # Promoted-channel HTTPS fetcher
│   │   ├── makapix_refresh.c            # Channel refresh
│   │   ├── makapix_single_artwork.c     # Single artwork playback
│   │   ├── view_tracker.c               # View tracking analytics
│   │   ├── makapix.h
│   │   ├── makapix_internal.h
│   │   ├── makapix_mqtt.h
│   │   ├── makapix_provision.h
│   │   ├── makapix_store.h
│   │   ├── makapix_api.h
│   │   ├── makapix_artwork.h
│   │   ├── makapix_certs.h
│   │   ├── makapix_promoted_https.h
│   │   ├── view_tracker.h
│   │   ├── include/view_tracker.h
│   │   ├── certs/
│   │   │   └── makapix_ca_cert.inc      # Embedded broker CA certificate
│   │   ├── .gitignore
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   │
│   ├── http_api/                        # HTTP server, REST API, WebSocket
│   │   ├── http_api.c                   # Server init, routing
│   │   ├── http_api_rest_status.c       # /status and /api/* endpoints
│   │   ├── http_api_rest_actions.c      # /action/* endpoints
│   │   ├── http_api_rest_settings.c     # /settings/* and /config endpoints
│   │   ├── http_api_rest_playsets.c     # /playsets/* CRUD
│   │   ├── http_api_rest_museum.c       # /api/museum/* rate-limit endpoints
│   │   ├── http_api_ota.c               # /ota/* endpoints
│   │   ├── http_api_upload.c            # /upload (multipart, 16 MiB cap)
│   │   ├── http_api_pages.c             # HTML page serving
│   │   ├── http_api_pico8.c             # /pico_stream WebSocket handler
│   │   ├── http_api_utils.c             # Utility functions
│   │   ├── http_api.h
│   │   ├── http_api_internal.h
│   │   ├── pico8_logo_data.h            # Embedded PICO-8 logo
│   │   ├── surrogate_ui.h               # Fallback UI when LittleFS web UI is missing
│   │   └── CMakeLists.txt
│   │
│   ├── config_store/                    # NVS-backed configuration
│   │   ├── config_store.c               # Core API
│   │   ├── config_store_settings.c      # General settings (rotation, dwell, etc.)
│   │   ├── config_store_giphy.c         # Giphy-specific settings
│   │   ├── config_store_klipy.c         # Klipy-specific settings
│   │   ├── config_store_internal.h
│   │   ├── config_store.h
│   │   └── CMakeLists.txt
│   │
│   ├── ota_manager/                     # OTA firmware and web UI updates
│   │   ├── ota_manager.c                # Lifecycle, periodic checks (default 12h)
│   │   ├── ota_manager_install.c        # Firmware installation
│   │   ├── ota_manager_webui.c          # Web UI (LittleFS) OTA
│   │   ├── github_ota.c                 # GitHub Releases API
│   │   ├── ota_manager_internal.h
│   │   ├── github_ota.h
│   │   ├── include/ota_manager.h
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   │
│   ├── slave_ota/                       # ESP32-C6 co-processor firmware auto-flash
│   │   ├── slave_ota.c
│   │   ├── firmware/
│   │   │   └── network_adapter.bin      # Embedded ESP32-C6 firmware blob
│   │   ├── include/slave_ota.h
│   │   └── CMakeLists.txt
│   │
│   ├── pico8/                           # PICO-8 streaming support
│   │   ├── pico8_stream.c               # WebSocket frame ingestion
│   │   ├── pico8_render.c               # 128x128 → 720x720 nearest-neighbor render
│   │   ├── pico8_audio.c                # Audio streaming (P3A_PICO8_AUDIO_ENABLE)
│   │   ├── pico8_stream_stubs.c         # Stubs when disabled
│   │   ├── include/
│   │   │   ├── pico8_stream.h
│   │   │   ├── pico8_render.h
│   │   │   ├── pico8_audio.h
│   │   │   └── pico8_logo_data.h
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   │
│   ├── content_cache/                   # Channel cache wrapper
│   │   ├── content_cache.c
│   │   ├── include/content_cache.h
│   │   └── CMakeLists.txt
│   │
│   ├── storage_eviction/                # SD card space management
│   │   ├── storage_eviction.c
│   │   ├── include/storage_eviction.h
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   │
│   ├── loader_service/                  # Animation file loader
│   │   ├── loader_service.c
│   │   ├── include/loader_service.h
│   │   └── CMakeLists.txt
│   │
│   ├── playback_queue/                  # Play scheduler → animation player adapter
│   │   ├── playback_queue.c
│   │   ├── include/playback_queue.h
│   │   └── CMakeLists.txt
│   │
│   ├── sdio_bus/                        # SDIO bus mutex coordinator
│   │   ├── sdio_bus.c
│   │   ├── include/sdio_bus.h
│   │   └── CMakeLists.txt
│   │
│   ├── show_url/                        # URL artwork downloader
│   │   ├── show_url.c
│   │   ├── include/show_url.h
│   │   └── CMakeLists.txt
│   │
│   ├── debug_http_log/                  # Performance instrumentation (compile-time optional)
│   │   ├── debug_http_log.c
│   │   ├── debug_http_log.h
│   │   └── CMakeLists.txt
│   │
│   ├── ugfx/                            # uGFX text/font rendering subset (DejaVu Sans 16/24/32)
│   └── libwebp_decoder/                 # libwebp v1.4.0 wrapper (FetchContent)
│
├── webui/                               # Web interface assets (packed into LittleFS)
│   ├── index.html                       # Main control page
│   ├── settings.html                    # Settings page
│   ├── giphy.html                       # Redirect stub → /settings#giphy
│   ├── ota.html                         # OTA update page
│   ├── playset-editor.html              # Playset editor page
│   ├── favicon.png                      # Brand-mark favicon (also served at legacy /favicon.ico)
│   ├── version.txt                      # Generated at configure time
│   ├── metadata.json                    # Generated at configure time (web UI version + API)
│   ├── config/
│   │   └── network.html                 # Network configuration page
│   ├── museum/                          # Museum browse adapters (ES modules, loaded by playset editor)
│   │   ├── index.js                     # Adapter registry + dispatch helpers
│   │   ├── browse.js                    # Modal flow (museum → axis → term → preview → add)
│   │   ├── artic.js                     # Art Institute of Chicago browse adapter
│   │   ├── rijksmuseum.js               # Rijksmuseum browse adapter
│   │   ├── rijks-sets.json              # Baked OAI-PMH set list (CORS workaround; see finalized-design §9.2)
│   │   ├── vam.js                       # Victoria and Albert Museum browse adapter
│   │   ├── wellcome.js                  # Wellcome Collection browse adapter
│   │   └── smk.js                       # Statens Museum for Kunst browse adapter
│   ├── setup/                           # Captive portal pages
│   │   ├── index.html                   # Wi-Fi setup form (POSTs to /save)
│   │   ├── success.html
│   │   ├── error.html
│   │   └── erased.html
│   ├── static/                          # Static assets
│   │   ├── common.css
│   │   ├── compat.js
│   │   ├── pico8.css
│   │   ├── pico8.js
│   │   ├── pico8_logo.png
│   │   ├── fake08.js                    # FAKE-08 emulator JS loader
│   │   └── fake08.wasm                  # FAKE-08 PICO-8 emulator (WebAssembly)
│   └── pico8/
│       └── index.html                   # PICO-8 monitor page
│
├── docs/                                # Documentation
│   ├── INFRASTRUCTURE.md                # Stub redirecting to docs/infrastructure/
│   ├── infrastructure/                  # Technical infrastructure docs (this folder)
│   ├── BOARD-CAPABILITIES.md            # Hardware capability sheet
│   ├── HOW-TO-USE.md                    # User guide
│   ├── flash-p3a.md                     # Flashing instructions
│   ├── deferred-features.md             # Tracker for doc claims pending implementation
│   ├── concurrent-tls-eagain-tabled.md  # Tabled-for-later technical note
│   ├── reference/                       # Protocol references (e.g., MQTT)
│   └── web-flasher/                     # Web flasher tool source
│
├── flasher/                             # Optional Windows flasher build (P3A_BUILD_FLASHER)
├── certs/                               # Provisioning certificate authority material
├── images/                              # Project art assets
│   ├── photos/                          # Device photos & demo captures (README, marketing)
│   ├── brand/                           # Logos & brand identity (PNG deliverables + source/ XCFs and raw sketches)
│   ├── screenshots/                     # Web UI screenshots
│   ├── pico-8/                          # PICO-8 logos, gameplay clips, web-interface shots
│   ├── ui-icons/                        # Source artwork for icons baked into firmware/webui
│   ├── hardware/                        # Board reference imagery
│   └── webui-manifest/                  # PWA / add-to-home-screen icons (source folder; LittleFS uses webui/static/)
├── scripts/                             # Build / development helper scripts
├── build/                               # Build output directory
├── release/                             # Per-version release binaries
├── managed_components/                  # Auto-downloaded ESP-IDF components (from idf_component.yml)
├── CMakeLists.txt                       # Root CMake configuration
├── partitions.csv                       # Flash partition layout
├── sdkconfig                            # ESP-IDF configuration (generated)
├── dependencies.lock                    # Pinned managed-component versions
├── README.md                            # User-facing documentation
├── LICENSE                              # Apache License 2.0
├── LICENSING.md                         # Dependency license report
├── CLAUDE.md                            # AI assistant guidance
└── AGENTS.md                            # AI agent instructions
```

## Key Files

- **CMakeLists.txt** (root): Build configuration, version variables (`PROJECT_VER`, `WEBUI_VERSION`, `P3A_API_VERSION`), LittleFS image creation, release packaging hook
- **partitions.csv**: Flash memory layout (NVS, dual OTA, LittleFS web UI, slave firmware)
- **sdkconfig**: ESP-IDF project configuration (generated from `menuconfig`)
- **dependencies.lock**: Authoritative pinned versions for ESP-IDF and managed components
