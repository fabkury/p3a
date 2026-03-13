# Meet p3a: An Open-Source Pixel Art Player Built on ESP32-P4

There's something magnetic about pixel art. Maybe it's the deliberate placement of every single dot, or the way a 64x64 grid can hold more personality than a 4K photograph. Either way, pixel art deserves better than a browser tab you'll forget about. It deserves a dedicated screen.

That's why we built **p3a** — a Wi-Fi-connected art frame powered by the ESP32-P4 that plays animated pixel art on a gorgeous 720x720 IPS touchscreen.

## What It Actually Does

p3a sits on your desk, shelf, or wall and cycles through animated artwork. It plays WebP, GIF, PNG, and JPEG files — with full transparency and alpha channel support. Small pixel art gets crisp nearest-neighbor upscaling that preserves those sharp edges, while photographic content gets smooth bilinear interpolation courtesy of the ESP32-P4's hardware pixel processing accelerator.

You load artwork onto a microSD card, connect to Wi-Fi, and it just runs. Tap the right side of the screen to skip forward, tap the left to go back, swipe up or down to adjust brightness. Two-finger rotate to change orientation. That's the whole interface. No app to install, no account required.

## But It's Not Just a Photo Frame

Here's where it gets interesting. p3a connects to three artwork sources:

**Local files** on the microSD card — your personal collection. Drop files into the `animations` folder, and they enter the rotation.

**Giphy trending** — plug in a free Giphy API key and p3a pulls trending GIFs automatically. Configure content rating, resolution, format, and refresh interval. The device downloads and caches GIFs on demand, so after the first play, everything is instant.

**Makapix Club** — a pixel art social network at [makapix.club](https://makapix.club/). Browse artworks on the website and send them directly to your p3a with one click. The device receives them instantly over a secure MQTT connection with mutual TLS authentication. You can also control playback, brightness, and navigation remotely from anywhere.

The **Play Scheduler** ties it all together. You create "playsets" — scheduler commands that define which channels to include, how to balance exposure across them, and how to pick artwork within each channel. Want 60% Makapix art, 30% local files, and 10% Giphy? Done.

## The Hardware

p3a runs on the Waveshare ESP32-P4-WIFI6-Touch-LCD-4B board:

- **ESP32-P4** dual-core RISC-V processor with PSRAM
- **ESP32-C6** co-processor providing Wi-Fi 6 and BLE
- **720x720 24-bit IPS display** connected via MIPI-DSI
- **GT911 capacitive touchscreen** with multi-touch gesture support
- **microSD slot** for artwork storage
- **Two USB-C ports** — one for power/flashing, one that exposes the SD card as a removable drive

The display pipeline uses triple buffering with VSYNC synchronization. Decoded frames pass through aspect-ratio-preserving scaling (with configurable background color for non-square artwork) before hitting the LCD via DMA. It's overkill for displaying pixel art, and that's exactly the point — buttery smooth playback with zero tearing.

## Web Interface and API

Once connected to Wi-Fi, p3a hosts a web interface at `http://p3a.local/`. You get playback controls, device status, configuration, Giphy settings, OTA firmware updates, and a playset editor — all from your browser.

Every feature exposed in the web UI is also available as a REST API. Want to integrate p3a into your Home Assistant setup? Wire it into a Stream Deck? Control it from a shell script? Just `curl` the endpoints.

```bash
# Skip to next artwork
curl -X POST http://p3a.local/action/swap_next

# Get device status as JSON
curl http://p3a.local/status
```

## Over-the-Air Updates

After the initial USB flash, you never need to plug in a cable again. p3a checks for firmware updates automatically and notifies you through the web interface. Updates install wirelessly in about two minutes. If a bad update fails to boot three times, the device automatically rolls back to the previous working firmware.

The ESP32-C6 co-processor firmware is managed transparently — when a new version is bundled with the main firmware, it gets flashed to the co-processor automatically at boot.

## One More Thing: PICO-8

p3a can also act as a wireless PICO-8 game display. Load a `.p8` cartridge in the browser, and the game streams to the device at 30 FPS via WebSocket. The 128x128 PICO-8 resolution upscales beautifully to 720x720 with nearest-neighbor scaling — every pixel is razor sharp. It automatically returns to art playback after 30 seconds of inactivity.

## Getting Started

All you need is the Waveshare board, a microSD card, a USB-C cable, and some artwork. Flash the firmware, connect to Wi-Fi through the captive portal, and you're up and running.

The firmware is built with ESP-IDF v5.5 and the full source is available on GitHub. If you're into embedded development, the codebase is organized as 24 modular ESP-IDF components — clean separation of concerns, well-defined interfaces, easy to extend.

p3a started as a weekend project to give pixel art the display it deserves. It grew into something we use every day. We think you might too.
