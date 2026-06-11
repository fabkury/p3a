# Project Overview

**p3a** is an ESP32-P4-powered Wi-Fi art frame that displays pixel art animations. It integrates:

- **Microcontroller**: Dual-core ESP32-P4 with ESP32-C6 for Wi-Fi 6/BLE
- **Display**: 720x720 pixel IPS touchscreen with 24-bit color
- **Storage**: microSD card via SDMMC interface
- **Connectivity**: Wi-Fi, HTTP server, WebSocket, REST API, mDNS, MQTT
- **I/O**: Capacitive touch, USB-C (CDC-ACM, Mass Storage, vendor bulk pipe)
- **Framework**: ESP-IDF v5.5.x

## Key Features

- **Designed for 24/7 operation** — both the hardware and the firmware are built to run continuously, with no scheduled reboots or daily power cycle required
- Animated WebP, GIF, and APNG, plus static PNG and JPEG playback
- **Transparency support** for WebP, GIF, APNG, and PNG with configurable background color
- **Aspect ratio preservation** for non-square artworks
- Touch gestures for navigation, Makapix reactions, info screen, and screen rotation
- Web-based control panel at `http://p3a.local/`
- **Giphy integration** — play trending GIFs from [Giphy](https://giphy.com/) with configurable content rating, rendition, and automatic refresh
- **Museum (IIIF) channels** — play public collections from the Art Institute of Chicago, Rijksmuseum, Victoria and Albert Museum, Wellcome Collection, and the Statens Museum for Kunst over [IIIF](https://iiif.io/); no API key required
- **Makapix Club integration** — send artworks directly from [makapix.club](https://makapix.club/)
- **Over-the-Air updates** — install firmware and web UI updates wirelessly via web UI
- **ESP32-C6 auto-flash** — co-processor firmware is updated automatically when needed
- **Play Scheduler** — deterministic multi-channel artwork selection with configurable playsets
- PICO-8 game monitor mode with WebSocket streaming
- USB Mass Storage for SD card access
- Captive portal for Wi-Fi provisioning
- **Show URL** — download and play artwork from arbitrary HTTP/HTTPS URLs
- **Playset editor** — web-based editor for creating and managing playsets

## Codebase Statistics

- **Custom components**: 25 ESP-IDF components under `components/`
- **Main application files**: ~24 C source files + 21 headers
- **Build artifacts**: ~7 MB (firmware ~2 MB + LittleFS web UI ~4 MB + ESP32-C6 firmware ~1 MB); reserves ~22 MB total flash including the second OTA slot
