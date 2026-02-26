# Directory Structure

```
p3a/
├── main/                             # Main application component
│   ├── p3a_main.c                   # Application entry point
│   ├── app_lcd_p4.c                 # LCD initialization and control
│   ├── app_touch.c                  # Touch input handling
│   ├── app_usb.c                    # USB composite device (conditional: P3A_USB_MSC_ENABLE)
│   ├── usb_descriptors.c           # USB device descriptors (conditional: P3A_USB_MSC_ENABLE)
│   ├── usb_descriptors.h           # USB descriptor definitions
│   ├── tusb_config.h               # TinyUSB configuration
│   ├── display_renderer.c          # Frame buffer management, vsync
│   ├── display_renderer_priv.h     # Display renderer private definitions
│   ├── display_upscaler.c          # Parallel CPU nearest-neighbor upscaling with rotation
│   ├── display_ppa_upscaler.c      # PPA hardware-accelerated bilinear upscaling (conditional: P3A_PPA_UPSCALE_ENABLE)
│   ├── display_fps_overlay.c       # FPS counter overlay
│   ├── display_processing_notification.c  # Swap processing/failure visual indicator
│   ├── render_engine.c             # Display rotation and background color API
│   ├── animation_player.c          # Core animation engine
│   ├── animation_player_render.c   # Frame rendering and composition
│   ├── animation_player_loader.c   # Asset loading
│   ├── animation_player_priv.h     # Animation player private definitions
│   ├── playback_controller.c       # Playback source management (animation, PICO-8, UI)
│   ├── connectivity_service.c      # Wi-Fi and OTA service wrapper
│   ├── content_service.c           # Content cache service wrapper
│   ├── playback_service.c          # Play scheduler service wrapper
│   ├── ugfx_ui.c                   # uGFX-based UI rendering
│   ├── CMakeLists.txt              # Main component build config
│   ├── Kconfig.projbuild           # Configuration menu items
│   ├── idf_component.yml           # ESP Component Registry manifest
│   ├── component.mk               # Legacy make compatibility
│   └── include/                    # Public headers
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
├── components/                      # Custom components (24 total)
│   ├── p3a_core/                   # Unified state machine and lifecycle
│   │   ├── p3a_state.c            # Global state machine
│   │   ├── p3a_render.c           # State-aware rendering dispatch
│   │   ├── p3a_touch_router.c     # Touch event routing by state
│   │   ├── p3a_logo.c             # Logo blitting utilities
│   │   ├── p3a_boot_logo.c        # Boot logo with fade-in
│   │   ├── sd_path.c              # SD card path management
│   │   ├── fresh_boot.c           # Debug NVS/SD erase utilities
│   │   ├── include/
│   │   │   ├── p3a_state.h
│   │   │   ├── p3a_render.h
│   │   │   ├── p3a_touch_router.h
│   │   │   ├── p3a_logo.h
│   │   │   ├── p3a_boot_logo.h
│   │   │   ├── sd_path.h
│   │   │   └── fresh_boot.h
│   │   └── CMakeLists.txt
│   │
│   ├── play_scheduler/             # Deterministic multi-channel playback engine
│   │   ├── play_scheduler.c       # Core scheduler logic
│   │   ├── play_scheduler_swrr.c  # Smooth Weighted Round Robin
│   │   ├── play_scheduler_commands.c  # Command execution
│   │   ├── play_scheduler_pick.c  # Artwork picking
│   │   ├── play_scheduler_navigation.c  # Next/prev navigation
│   │   ├── play_scheduler_timer.c # Dwell time timer
│   │   ├── play_scheduler_nae.c   # New Artwork Events
│   │   ├── play_scheduler_lai.c   # Last Access Index tracking
│   │   ├── play_scheduler_refresh.c   # Channel refresh
│   │   ├── play_scheduler_buffers.c   # Buffer management
│   │   ├── play_scheduler_cache.c # Cache integration
│   │   ├── playset_store.c        # Playset persistence (NVS)
│   │   ├── playset_json.c         # Playset JSON serialization
│   │   ├── include/
│   │   │   ├── play_scheduler.h
│   │   │   ├── play_scheduler_internal.h
│   │   │   ├── play_scheduler_types.h
│   │   │   ├── playset_store.h
│   │   │   └── playset_json.h
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   │
│   ├── event_bus/                  # Asynchronous event pub/sub
│   │   ├── event_bus.c
│   │   ├── include/event_bus.h
│   │   └── CMakeLists.txt
│   │
│   ├── p3a_board_ep44b/           # Board abstraction (EP44B hardware)
│   │   ├── p3a_board_display.c    # Display hardware init
│   │   ├── p3a_board_fs.c         # LittleFS and SD card
│   │   ├── p3a_board_button.c     # BOOT button support
│   │   ├── include/p3a_board.h    # Public board API
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   │
│   ├── channel_manager/            # Channel/playlist management
│   │   ├── sdcard_channel.c       # SD card channel
│   │   ├── sdcard_channel_impl.c  # SD card implementation
│   │   ├── makapix_channel_impl.c # Makapix channel implementation
│   │   ├── makapix_channel_events.c   # Makapix event handling
│   │   ├── makapix_channel_refresh.c  # Makapix channel refresh
│   │   ├── makapix_channel_utils.c    # Makapix channel utilities
│   │   ├── vault_storage.c        # SHA256-sharded artwork storage
│   │   ├── animation_metadata.c   # JSON sidecar metadata
│   │   ├── channel_cache.c        # Channel cache
│   │   ├── channel_metadata.c     # Channel metadata
│   │   ├── channel_settings.c     # Channel settings
│   │   ├── download_manager.c     # Download coordination
│   │   ├── playlist_manager.c     # Playlist management
│   │   ├── include/
│   │   │   ├── channel_interface.h
│   │   │   ├── sdcard_channel.h
│   │   │   ├── sdcard_channel_impl.h
│   │   │   ├── makapix_channel_impl.h
│   │   │   ├── makapix_channel_events.h
│   │   │   ├── makapix_channel_utils.h
│   │   │   ├── vault_storage.h
│   │   │   ├── animation_metadata.h
│   │   │   ├── channel_cache.h
│   │   │   ├── channel_metadata.h
│   │   │   ├── channel_settings.h
│   │   │   ├── download_manager.h
│   │   │   ├── playlist_manager.h
│   │   │   ├── animation_swap_request.h
│   │   │   ├── psram_alloc.h
│   │   │   ├── pcg32_reversible.h
│   │   │   └── uthash.h
│   │   ├── makapix_channel_internal.h
│   │   ├── pcg32_reversible.h
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   │
│   ├── wifi_manager/               # Wi-Fi and SNTP
│   │   ├── app_wifi.c             # Wi-Fi STA/AP, captive portal
│   │   ├── sntp_sync.c            # NTP time synchronization
│   │   ├── include/
│   │   │   ├── app_wifi.h
│   │   │   └── sntp_sync.h
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   │
│   ├── animation_decoder/          # Image/animation decoders
│   │   ├── webp_animation_decoder.c
│   │   ├── png_animation_decoder.c
│   │   ├── jpeg_animation_decoder.c
│   │   ├── static_image_decoder_common.h
│   │   ├── include/
│   │   │   ├── animation_decoder.h
│   │   │   └── animation_decoder_internal.h
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   │
│   ├── animated_gif_decoder/       # GIF decoder (C++ wrapper)
│   │   ├── AnimatedGIF.cpp
│   │   ├── gif_animation_decoder.cpp
│   │   ├── gif.inl
│   │   └── CMakeLists.txt
│   │
│   ├── giphy/                      # Giphy API integration
│   │   ├── giphy_api.c            # API calls
│   │   ├── giphy_cache.c          # SD card cache
│   │   ├── giphy_download.c       # Download with atomic writes
│   │   ├── giphy_refresh.c        # Periodic refresh
│   │   ├── include/
│   │   │   ├── giphy.h
│   │   │   └── giphy_types.h
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   │
│   ├── makapix/                    # Makapix Club integration
│   │   ├── makapix.c              # Module init and lifecycle
│   │   ├── makapix_mqtt.c         # MQTT client
│   │   ├── makapix_provision.c    # Device provisioning
│   │   ├── makapix_provision_flow.c   # Provisioning state machine
│   │   ├── makapix_store.c        # NVS credential storage
│   │   ├── makapix_api.c          # HTTPS API calls
│   │   ├── makapix_artwork.c      # Artwork receiving
│   │   ├── makapix_certs.c        # TLS certificates
│   │   ├── makapix_connection.c   # Connection management
│   │   ├── makapix_channel_switch.c   # Channel switching
│   │   ├── makapix_refresh.c      # Channel refresh
│   │   ├── makapix_single_artwork.c   # Single artwork playback
│   │   ├── view_tracker.c         # View tracking analytics
│   │   ├── makapix.h
│   │   ├── makapix_internal.h
│   │   ├── makapix_mqtt.h
│   │   ├── makapix_provision.h
│   │   ├── makapix_store.h
│   │   ├── makapix_api.h
│   │   ├── makapix_artwork.h
│   │   ├── makapix_certs.h
│   │   ├── view_tracker.h
│   │   ├── .gitignore
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   │
│   ├── http_api/                   # HTTP server and REST API
│   │   ├── http_api.c             # Server init, routing
│   │   ├── http_api_rest_status.c # Status endpoint
│   │   ├── http_api_rest_actions.c    # Action endpoints
│   │   ├── http_api_rest_settings.c   # Settings endpoints
│   │   ├── http_api_rest_playsets.c   # Playset CRUD endpoints
│   │   ├── http_api_ota.c         # OTA endpoints
│   │   ├── http_api_upload.c      # File upload
│   │   ├── http_api_pages.c       # HTML page serving
│   │   ├── http_api_pico8.c       # PICO-8 WebSocket
│   │   ├── http_api_utils.c       # Utility functions
│   │   ├── http_api.h
│   │   ├── http_api_internal.h
│   │   ├── favicon_data.h
│   │   ├── pico8_logo_data.h
│   │   ├── surrogate_ui.h
│   │   └── CMakeLists.txt
│   │
│   ├── config_store/               # NVS-backed configuration
│   │   ├── config_store.c
│   │   ├── config_store.h
│   │   └── CMakeLists.txt
│   │
│   ├── ota_manager/                # OTA firmware updates
│   │   ├── ota_manager.c          # Manager lifecycle, periodic checks
│   │   ├── ota_manager_install.c  # Firmware installation
│   │   ├── ota_manager_webui.c    # Web UI OTA updates
│   │   ├── github_ota.c           # GitHub Releases API
│   │   ├── ota_manager_internal.h
│   │   ├── github_ota.h
│   │   ├── include/ota_manager.h
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   │
│   ├── slave_ota/                  # ESP32-C6 co-processor firmware
│   │   ├── slave_ota.c
│   │   ├── include/slave_ota.h
│   │   └── CMakeLists.txt
│   │
│   ├── pico8/                      # PICO-8 streaming support
│   │   ├── pico8_stream.c         # WebSocket streaming
│   │   ├── pico8_render.c         # 128x128 frame rendering
│   │   ├── pico8_stream_stubs.c   # Stubs when disabled
│   │   ├── include/
│   │   │   ├── pico8_stream.h
│   │   │   ├── pico8_render.h
│   │   │   └── pico8_logo_data.h
│   │   ├── CMakeLists.txt
│   │   └── Kconfig
│   │
│   ├── content_cache/              # Channel cache wrapper
│   │   ├── content_cache.c
│   │   ├── include/content_cache.h
│   │   └── CMakeLists.txt
│   │
│   ├── content_source/             # Channel content source abstraction
│   │   ├── content_source.c
│   │   ├── include/content_source.h
│   │   └── CMakeLists.txt
│   │
│   ├── loader_service/             # Animation file loader
│   │   ├── loader_service.c
│   │   ├── include/loader_service.h
│   │   └── CMakeLists.txt
│   │
│   ├── playback_queue/             # Play scheduler to animation player adapter
│   │   ├── playback_queue.c
│   │   ├── include/playback_queue.h
│   │   └── CMakeLists.txt
│   │
│   ├── sdio_bus/                   # SDIO bus coordinator
│   │   ├── sdio_bus.c
│   │   ├── include/sdio_bus.h
│   │   └── CMakeLists.txt
│   │
│   ├── show_url/                   # URL artwork downloader
│   │   ├── show_url.c
│   │   ├── include/show_url.h
│   │   └── CMakeLists.txt
│   │
│   ├── debug_http_log/             # Performance instrumentation (compile-time optional)
│   │   ├── debug_http_log.c
│   │   ├── debug_http_log.h
│   │   └── CMakeLists.txt
│   │
│   ├── app_state/                  # Application state (legacy, see p3a_core)
│   ├── ugfx/                       # uGFX graphics library
│   └── libwebp_decoder/            # libwebp wrapper
│
├── webui/                          # Web interface files
│   ├── index.html                 # Main control page
│   ├── settings.html              # Settings page
│   ├── giphy.html                 # Giphy settings page
│   ├── ota.html                   # OTA update page
│   ├── playset-editor.html        # Playset editor page
│   ├── metadata.json              # Web UI version and API compatibility
│   ├── config/
│   │   └── network.html           # Network configuration page
│   ├── setup/                     # Captive portal pages
│   │   ├── index.html             # Wi-Fi setup form
│   │   ├── success.html
│   │   ├── error.html
│   │   └── erased.html
│   ├── static/                    # Static assets
│   │   ├── compat.js
│   │   ├── pico8.css
│   │   └── pico8.js
│   └── pico8/
│       └── index.html             # PICO-8 web interface
│
├── docs/                           # Documentation
│   ├── infrastructure/            # Technical infrastructure docs (this folder)
│   ├── BOARD-CAPABILITIES.md
│   ├── HOW-TO-USE.md
│   ├── flash-p3a.md
│   ├── state-diagrams/            # State machine diagrams
│   ├── reference/                 # Protocol references (MQTT)
│   ├── playset-editor/            # Playset editor spec
│   ├── instructables/             # Instructables article
│   ├── web-flasher/               # Web flasher tool
│   ├── first-principles/          # Architecture design docs
│   └── dead-code/                 # Dead code analysis
│
├── build/                          # Build output directory
├── release/                        # Release binaries (per version)
├── managed_components/             # Auto-downloaded ESP-IDF components
├── CMakeLists.txt                 # Root CMake configuration
├── partitions.csv                 # Flash partition layout
├── sdkconfig                      # ESP-IDF configuration (generated)
├── README.md                      # User-facing documentation
├── LICENSE
├── CLAUDE.md                      # AI assistant guidance
└── AGENTS.md                      # AI agent instructions
```

## Key Files

- **CMakeLists.txt** (root): Build configuration, versioning, LittleFS image creation, release packaging
- **partitions.csv**: Flash memory layout (NVS, dual OTA, LittleFS, slave firmware)
- **sdkconfig**: ESP-IDF project configuration (auto-generated from menuconfig)
