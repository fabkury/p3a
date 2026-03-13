<p align="center">
  <img src="images/p3a-alpha-x2-128p.png" alt="p3a logo" width="128" height="128" />
</p>

<h1 align="center">p3a</h1>
<p align="center"><strong>A Wi-Fi pixel art player for the ESP32-P4</strong></p>
<p align="center">
  Animated art on your desk. Trending GIFs from Giphy, community artworks from Makapix Club, and your own files — all on a vivid 4-inch IPS touchscreen you can control from across the room or across the world.
</p>

<p align="center">
  <img src="images/p3a-1.jpg" alt="p3a front" height="260">
  &nbsp;
  <img src="images/p3a-7.jpg" alt="p3a angled" height="260">
  &nbsp;
  <img src="images/p3a-4-giphy.jpg" alt="p3a playing a Giphy GIF" height="260">
</p>

> **Join the community:** [Makapix Club Discord](https://discord.gg/xk9umcujXV)

---

## What is p3a?

p3a turns a [$60 development board](https://www.waveshare.com/product/arduino/boards-kits/esp32-p4/esp32-p4-wifi6-touch-lcd-4b.htm?sku=31416) into a smart art frame. Flash the firmware, connect to Wi-Fi, and it starts playing animated pixel art, GIFs, and still images on a crisp 720x720 display.

It's open source, self-contained, and designed to be as simple to use as a picture frame — while still exposing a full REST API for those who want to automate and tinker.

### Three content sources, one device

| Source | What you get |
|--------|--------------|
| **[Makapix Club](https://makapix.club/)** | Browse a pixel art social network and send artworks straight to your p3a — from anywhere |
| **[Giphy](https://giphy.com/)** | Trending GIFs auto-downloaded and cached locally, refreshed throughout the day |
| **Your own files** | Copy WebP, GIF, PNG, or JPEG via USB — they just play |

Mix all three in the same playlist. p3a shuffles between them automatically.

<p align="center">
  <img src="images/p3a_10fps.gif" alt="p3a playing animated artworks" height="300">
</p>

---

## Quick Start

1. **Get the hardware**
   - [Waveshare ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/product/arduino/boards-kits/esp32-p4/esp32-p4-wifi6-touch-lcd-4b.htm?sku=31416)
   - A [microSD card](https://www.waveshare.com/micro-sd-card-16gb-kawau.htm?sku=18191), a USB-C cable, and a small screwdriver
2. **Insert the microSD card** (requires unscrewing the back plate)
3. **Flash the firmware** using the [p3a Web Flasher](https://fabkury.github.io/p3a/web-flasher/) — connect the board and click. No toolchain needed. ([Alternative methods](docs/flash-p3a.md))
4. **Connect to Wi-Fi** — on first boot, join the `p3a-setup` network and configure at `http://p3a.local/`
5. **Start playing** — add a Giphy API key for trending GIFs, send artworks from [makapix.club](https://makapix.club/), or copy your own files over USB

> The initial flash requires a USB connection to a computer. After that, all updates are wireless.

For the full walkthrough, see [HOW-TO-USE.md](docs/HOW-TO-USE.md).

---

## Features

### Playback

- **Gapless, freeze-free rendering** — triple-buffered pipeline with VSYNC handles even problematic files gracefully
- **Animated WebP, GIF, PNG, JPEG** — transparency and alpha channel fully supported
- **Smart upscaling** — pixel art uses nearest-neighbor scaling to keep edges crisp; Giphy content uses hardware-accelerated bilinear interpolation for smooth results
- **Aspect ratio preservation** — non-square artwork is centered on the 720x720 display with configurable background color
- **Auto-advance** — cycles to a new artwork every 30 seconds (configurable)

### Control

- **Touchscreen** — tap to change artwork, swipe to adjust brightness, two-finger rotate to change orientation
- **Web UI** — full dashboard at `http://p3a.local/` from any browser on your network
- **REST API** — JSON endpoints for automation and scripting
- **Remote via Makapix Club** — register your device and control it from anywhere over secure MQTTS

### Giphy Integration

- Fetches trending GIFs automatically via the Giphy API
- Configurable content rating (G through R), format (WebP or GIF), rendition size, and refresh interval
- GIFs are cached on the SD card — once downloaded, playback is instant

<p align="center">
  <img src="images/p3a-3-giphy.jpg" alt="p3a playing a Giphy GIF" height="300">
</p>

### Makapix Club

[Makapix Club](https://makapix.club/) is a pixel art social network. Register your p3a to:

- Browse and send artworks or entire channels directly to your device
- Control your p3a remotely — change artwork, adjust brightness, pause and resume
- Receive artworks and commands in real time over TLS-secured MQTT

### Over-the-Air Updates

- One-click firmware updates from the web UI — no USB needed after the first flash
- Automatic update checks every 2 hours; updates are never installed without your approval
- Automatic rollback if a new firmware fails to boot three times
- SHA256 verification, ESP32-C6 co-processor auto-update, and web UI updates all included

<p align="center">
  <a href="images/PXL_20251206_184110573_red.mp4">
    <img src="images/ota_updates.png" alt="OTA update interface" width="380">
  </a>
  <br>
  <em>OTA update page (click to watch video)</em>
</p>

### PICO-8 Monitor

Stream PICO-8 games to the display over Wi-Fi. Load a `.p8` cart in the browser, and frames are sent to the device at 30 FPS via WebSocket — a dedicated retro-gaming screen on your desk.

<p align="center">
  <img src="images/pico-8-gameplay-2.gif" alt="PICO-8 gameplay on p3a">
</p>

---

## Hardware

| | |
|---|---|
| **Board** | [Waveshare ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/product/arduino/boards-kits/esp32-p4/esp32-p4-wifi6-touch-lcd-4b.htm?sku=31416) |
| **MCU** | Dual-core ESP32-P4 + ESP32-C6 Wi-Fi 6 / BLE co-processor |
| **Display** | 4" 720x720 IPS, 24-bit color, dimmable backlight |
| **Touch** | 5-point capacitive (GT911) |
| **Memory** | 32 MB PSRAM, 32 MB flash |
| **Storage** | microSD card slot |
| **Power** | USB-C (no battery needed) |

<p align="center">
  <img src="images/ESP32-P4-WIFI6-Touch-LCD-4B-details-size.jpg" alt="Board dimensions" width="100%">
</p>

---

## Documentation

| Document | Description |
|----------|-------------|
| [HOW-TO-USE.md](docs/HOW-TO-USE.md) | Complete usage guide — setup, touch controls, Wi-Fi, Giphy, PICO-8, and more |
| [flash-p3a.md](docs/flash-p3a.md) | Flashing instructions (web flasher + alternative methods) |
| [INFRASTRUCTURE.md](docs/INFRASTRUCTURE.md) | Technical architecture for developers and contributors |

## Repository Layout

```
main/           Application core — boot, display, animation pipeline, decoders
components/     Custom ESP-IDF components — state machine, HTTP API, Makapix,
                OTA, Giphy, config store, Wi-Fi manager, and more
managed_components/  ESP-IDF Component Registry dependencies
webui/          Web interface (served from LittleFS)
docs/           Documentation
```

---

## Building from Source

p3a is built with ESP-IDF v5.5. If you just want to use p3a, you don't need to build anything — use the [web flasher](https://fabkury.github.io/p3a/web-flasher/).

```bash
# Activate ESP-IDF environment (example for Windows PowerShell)
C:\Users\Fab\esp\v5.5.1\esp-idf\export.ps1

# Set target (first time only)
idf.py set-target esp32p4

# Build
idf.py build

# Flash and monitor
idf.py flash monitor
```

---

## License

Apache License 2.0. See [LICENSE](LICENSE) for the full text and [LICENSING.md](LICENSING.md) for third-party dependency licenses.

## Contributing

Contributions are welcome. By submitting a contribution, you agree to license it under the Apache License 2.0. See [INFRASTRUCTURE.md](docs/INFRASTRUCTURE.md) for technical details about the codebase.

## Acknowledgements

Thanks to ByteWelder from the *Espressif MCUs* Discord channel for advice very early in the p3a project.
