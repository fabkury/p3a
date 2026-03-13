<img src="images/p3a-alpha-x2-128p.png" alt="p3a" align="left" width="128" height="128" />

# p3a — Pixel Art Player

**A 4-inch smart art frame powered by ESP32-P4.** Plays animated pixel art from [Makapix Club](https://makapix.club/), trending GIFs from [Giphy](https://giphy.com/), and your own files — all on a vivid 720×720 IPS touchscreen you control from anywhere.

<br clear="left" />

<p align="center">
  <img src="images/p3a-1.jpg" alt="p3a front" height="300">
  &nbsp;
  <img src="images/p3a-7.jpg" alt="p3a angled" height="300">
  &nbsp;
  <img src="images/p3a-4-giphy.jpg" alt="p3a playing a Giphy GIF" height="300">
</p>

<p align="center">
  <a href="https://discord.gg/xk9umcujXV"><strong>Discord</strong></a> · <a href="https://makapix.club/"><strong>Makapix Club</strong></a> · <a href="docs/HOW-TO-USE.md"><strong>User Guide</strong></a> · <a href="https://fabkury.github.io/p3a/web-flasher/"><strong>Web Flasher</strong></a>
</p>

---

## What is p3a?

p3a turns a Waveshare ESP32-P4 board into a dedicated pixel art display. Think of it as a tiny gallery on your desk — it cycles through artwork automatically, picks up trending GIFs throughout the day, and lets artists send creations directly to it from [Makapix Club](https://makapix.club/), a pixel art social network.

You control it by tapping and swiping the touchscreen, from a web browser on your local network, through a REST API, or remotely from anywhere via Makapix Club's secure cloud backend.

<p align="center">
  <img src="images/p3a_10fps.gif" alt="p3a playing animated artworks" height="300">
</p>

## Quick Start

1. **Get the hardware** — [Waveshare ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/product/arduino/boards-kits/esp32-p4/esp32-p4-wifi6-touch-lcd-4b.htm?sku=31416), a [microSD card](https://www.waveshare.com/micro-sd-card-16gb-kawau.htm?sku=18191), a USB-C cable, and a small screwdriver
2. **Insert the microSD card** — requires unscrewing the back plate
3. **Flash the firmware** — visit the [p3a Web Flasher](https://fabkury.github.io/p3a/web-flasher/) (connect, click, done)
4. **Connect to Wi-Fi** — on first boot, join the `p3a-setup` network and open http://p3a.local/
5. **Register at Makapix Club** — follow the instructions in the settings page to unlock cloud features
6. **Play** — add a free [Giphy API key](https://developers.giphy.com/) for trending GIFs, browse [makapix.club](https://makapix.club/) to send artworks, or copy your own files via USB

> The initial flash requires USB and a computer. After that, all updates are wireless.

For full setup details, see the [User Guide](docs/HOW-TO-USE.md). For alternative flashing methods, see [flash-p3a.md](docs/flash-p3a.md).

---

## Features

### Smooth, Format-Flexible Playback

Animated WebP, animated GIF, PNG (with full alpha), and hardware-accelerated JPEG — all with transparency support and configurable background color. Triple-buffered rendering with VSYNC keeps animations smooth and gapless, even with problematic files.

Pixel art from Makapix and local files is upscaled with **nearest-neighbor scaling** to keep pixel edges crisp. Giphy content uses **hardware-accelerated bilinear interpolation** for smooth results. Non-square artwork is always centered and aspect-ratio-correct.

### Makapix Club — A Pixel Art Social Network

<p align="center">
  <img src="images/p3a-2.jpg" alt="p3a displaying artwork" height="300">
</p>

[Makapix Club](https://makapix.club/) is a social network for pixel artists. Register your p3a to:

- **Browse and send** artworks or entire channels (e.g. "Promoted Artworks", "Recent Artworks") straight to your device
- **Control remotely** — change artwork, adjust brightness from anywhere via secure MQTTS
- **Stay connected** — your device receives artwork updates in real time

> Coming soon: "like" artworks with a long-press, swipe up to view community comments.

### Giphy — Trending GIFs on Your Desk

<p align="center">
  <img src="images/p3a-3-giphy.jpg" alt="p3a playing a Giphy GIF" height="300">
</p>

Add a free API key and p3a starts pulling trending GIFs automatically — animated memes, reactions, and pop culture cycling on your display all day. Configure rendition size, file format, content rating, and refresh interval from the built-in settings page. Mix Giphy channels with Makapix artwork in the same playlist for a varied, ever-changing display.

### Touch Controls

| Gesture | Action |
|---------|--------|
| Tap right half | Next artwork |
| Tap left half | Previous artwork |
| Swipe up / down | Adjust brightness |
| Two-finger rotate | Rotate screen (0° / 90° / 180° / 270°) |
| Long press | Start device registration |

Rotation persists across reboots. All controls are also available via the web UI and REST API.

### Web Interface & REST API

Open `http://p3a.local/` from any browser on the same Wi-Fi network for a full dashboard — playback controls, configuration, firmware updates, Giphy settings, and more. Every action is also exposed as a JSON API endpoint for scripting and automation.

```bash
curl http://p3a.local/status                        # Device status
curl -X POST http://p3a.local/action/swap_next      # Next artwork
curl -X POST http://p3a.local/api/rotation \
  -H "Content-Type: application/json" \
  -d '{"rotation": 90}'                             # Rotate screen
```

> The web interface is LAN-only. For remote control, use Makapix Club.

### Over-the-Air Updates

After the first USB flash, all updates are wireless. p3a checks for new firmware every 2 hours and shows a notification in the web UI when an update is available — you always approve manually. If a new firmware fails to boot three times, the device **automatically rolls back** to the previous working version.

Starting with v0.7.5, the web UI itself can also be updated over-the-air.

<p align="center">
  <a href="images/PXL_20251206_184110573_red.mp4">
    <img src="images/ota_updates.png" alt="OTA update web interface" width="400">
  </a>
  <br>
  <em>OTA update page (click to watch video)</em>
</p>

### USB Storage & PICO-8

Connect via USB-C to mount the microSD card as a removable drive — drag and drop your own artwork files. p3a can also act as a wireless PICO-8 game monitor: load a `.p8` cart in the browser-based emulator and the game streams to the display at 30 FPS over WebSocket.

<p align="center">
  <img src="images/pico-8-gameplay-2.gif" alt="PICO-8 gameplay on p3a" height="280">
</p>

---

## Hardware

p3a runs on the [Waveshare ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/product/arduino/boards-kits/esp32-p4/esp32-p4-wifi6-touch-lcd-4b.htm?sku=31416):

| | |
|---|---|
| **MCU** | Dual-core ESP32-P4 + ESP32-C6 (Wi-Fi 6 / BLE) |
| **Display** | 4" 720×720 IPS, 24-bit color, dimmable backlight |
| **Touch** | 5-point capacitive (GT911) |
| **Memory** | 32 MB PSRAM, 32 MB flash |
| **Storage** | microSD slot (SDMMC) |
| **Power** | USB-C (no battery) |

<p align="center">
  <img src="images/ESP32-P4-WIFI6-Touch-LCD-4B-details-size.jpg" alt="Board dimensions" width="100%">
</p>

---

## For Developers

### Architecture at a Glance

p3a is built on **ESP-IDF v5.5.x** with 24 custom components. The core pipeline:

```
Boot → NVS/LittleFS → LCD/Touch/USB/WiFi → HTTP Server → Makapix MQTT
         ↓
   Play Scheduler → Animation Loader → Decoder (WebP/GIF/PNG/JPEG)
         ↓
   Triple-buffered Renderer → VSYNC → Display
```

Key entry points:
- **`main/p3a_main.c`** — boot sequence and initialization
- **`main/animation_player.c`** — decode/render pipeline
- **`main/display_renderer.c`** — triple-buffered frame management
- **`main/playback_controller.c`** — switches between animation, PICO-8, and UI render sources

### Repository Layout

```
main/           Application core — display, touch, animation player, decoders
components/     24 custom ESP-IDF components (state machine, HTTP API,
                Makapix client, OTA manager, Giphy, decoders, etc.)
managed_components/   ESP-IDF Component Registry dependencies
webui/          Web interface (compiled into LittleFS)
docs/           Documentation
release/        Release binaries
```

### Storage Layout

| Partition | Mount | Size | Purpose |
|-----------|-------|------|---------|
| LittleFS | `/webui` | 4 MB | Web UI assets |
| NVS | — | 24 KB | Wi-Fi credentials, settings, state |
| OTA 0 / OTA 1 | — | 8 MB each | Dual firmware slots |
| Slave FW | — | 2 MB | ESP32-C6 co-processor firmware |
| **SD Card** | `/sdcard` | — | All artwork storage |

SD card layout: `/sdcard/p3a/animations/` (local files), `/sdcard/p3a/vault/` (Makapix cache, SHA256-sharded), `/sdcard/p3a/giphy/` (Giphy cache).

### Documentation

| Document | Description |
|----------|-------------|
| [HOW-TO-USE.md](docs/HOW-TO-USE.md) | Complete user guide |
| [flash-p3a.md](docs/flash-p3a.md) | Flashing instructions |
| [INFRASTRUCTURE.md](docs/INFRASTRUCTURE.md) | Full technical architecture |

---

## Community

- **Discord** — [Makapix Club Discord](https://discord.gg/xk9umcujXV) for discussion, tips, and pixel art
- **Makapix Club** — [makapix.club](https://makapix.club/) to browse and share pixel art
- **Issues** — [GitHub Issues](../../issues) for bug reports and feature requests

## Contributing

Contributions are welcome. By contributing to p3a, you agree that your contributions will be licensed under the Apache License 2.0. See [INFRASTRUCTURE.md](docs/INFRASTRUCTURE.md) for technical details about the codebase.

## License

Apache License 2.0. See [LICENSE](LICENSE) for the full text and [LICENSING.md](LICENSING.md) for dependency licenses.

## Acknowledgements

Thanks to ByteWelder from the *Espressif MCUs* Discord channel for advice early in the p3a project.
