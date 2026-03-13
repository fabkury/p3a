<img src="images/p3a-alpha-x2-128p.png" alt="p3a" align="left" width="128" height="128" />

# p3a

### A smart pixel art frame powered by ESP32-P4

Animated GIFs, pixel art, and PICO-8 games on a 720x720 touchscreen you can control from anywhere.

<br clear="left"/>

---

<p align="center">
  <img src="images/p3a-1.jpg" alt="p3a displaying pixel art" height="280">
  &nbsp;
  <img src="images/p3a-7.jpg" alt="p3a on a desk" height="280">
  &nbsp;
  <img src="images/p3a-4-giphy.jpg" alt="p3a playing a Giphy GIF" height="280">
</p>

p3a turns a [Waveshare ESP32-P4 board](https://www.waveshare.com/product/arduino/boards-kits/esp32-p4/esp32-p4-wifi6-touch-lcd-4b.htm?sku=31416) into a desk-sized pixel art display. It plays trending GIFs from [Giphy](https://giphy.com/), animated artworks from [Makapix Club](https://makapix.club/) (a pixel art social network), and your own files. Control it by touch, from a browser on your local network, or remotely from anywhere through Makapix Club.

> **Community:** Join the [Makapix Club Discord](https://discord.gg/xk9umcujXV) to connect with other p3a users and talk pixel art.

## Quick Start

| What you need | |
|---|---|
| [Waveshare ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/product/arduino/boards-kits/esp32-p4/esp32-p4-wifi6-touch-lcd-4b.htm?sku=31416) | The board itself |
| [microSD card](https://www.waveshare.com/micro-sd-card-16gb-kawau.htm?sku=18191) | For artwork storage |
| USB-C cable | For initial flash |
| Small screwdriver | To open the back plate for the SD card |

1. **Insert the microSD card** — unscrew the back plate and slot it in
2. **Flash the firmware** — visit the [p3a Web Flasher](https://fabkury.github.io/p3a/web-flasher/), connect your board, and click to flash. No software to install. ([Alternative methods](docs/flash-p3a.md))
3. **Connect to Wi-Fi** — on first boot, join the `p3a-setup` network and open http://p3a.local/ to enter your Wi-Fi credentials
4. **Start playing art** — add a [Giphy API key](https://developers.giphy.com/) for trending GIFs, browse [makapix.club](https://makapix.club/) to send artworks, or copy your own files via USB

> The initial flash needs a USB connection to a computer. After that, everything is wireless.

For the full walkthrough, see [HOW-TO-USE.md](docs/HOW-TO-USE.md).

## What It Does

### Giphy: Trending GIFs on Your Desk

p3a pulls trending GIFs straight from the Giphy API and loops them all day — memes, reactions, pop culture moments cycling on your display without you lifting a finger. Configure the rendition size, file format (WebP or GIF), and content rating (G through R). The feed refreshes automatically.

<p align="center">
  <img src="images/p3a-3-giphy.jpg" alt="p3a playing a Giphy GIF" height="300">
</p>

### Makapix Club: Pixel Art from a Community of Artists

[Makapix Club](https://makapix.club/) is a social network for pixel art. Register your p3a there to stream individual artworks or entire channels — "Promoted Artworks", "Recent Artworks", or curated collections — directly to the display. Control arrives in real time over a secure MQTTS connection, so you can push new art to your frame from anywhere in the world.

<p align="center">
  <img src="images/p3a-2.jpg" alt="p3a showing Makapix artwork" height="300">
</p>

### Your Own Files

Plug in a USB-C cable and the microSD card shows up as a drive. Drop in animated WebP, GIF, PNG, or JPEG files and p3a plays them. No cloud account needed, no internet required.

### Playback That Just Works

- **Gapless, freeze-free rendering** with triple buffering and VSYNC
- **Full format support**: animated WebP, animated GIF, PNG, and JPEG — all with transparency/alpha
- **Smart scaling**: hardware-accelerated bilinear interpolation for Giphy content, nearest-neighbor for pixel art (so edges stay crisp)
- **Aspect ratio preservation** with configurable background color
- **Mix and match** — combine Giphy channels and Makapix channels in a single playset for a varied, ever-changing display

<p align="center">
  <img src="images/p3a_10fps.gif" alt="p3a playing animated artworks" height="300">
</p>

### Touch & Control

- **Tap** to skip to the next artwork
- **Swipe** to adjust brightness
- **Two-finger rotate** the display (0/90/180/270 degrees)
- **Web UI** at `http://p3a.local/` for full control from any browser on your LAN
- **REST API** for automation and integrations
- **Remote control** from anywhere via Makapix Club

### PICO-8 Monitor

Stream [PICO-8](https://www.lexaloffle.com/pico-8.php) games to the display over Wi-Fi. The 720x720 screen is a perfect square canvas for PICO-8's 128x128 output.

<p align="center">
  <img src="images/pico-8-gameplay-2.gif" alt="PICO-8 game running on p3a" height="280">
</p>

## Over-the-Air Updates

After the initial USB flash, all future updates are wireless. p3a checks for new releases from GitHub every 2 hours — or you can check manually from the web UI.

- **One-click install** from `http://p3a.local/ota`
- **Automatic rollback** if new firmware fails to boot 3 times
- **Manual rollback** to the previous version at any time
- **SHA256 verification** of every download
- **ESP32-C6 auto-update** — the Wi-Fi co-processor firmware updates itself when needed
- **Web UI updates** are also delivered over-the-air (since v0.7.5)

<p align="center">
  <a href="images/PXL_20251206_184110573_red.mp4">
    <img src="images/ota_updates.png" alt="OTA update interface" width="380">
  </a>
  <br>
  <em>OTA update page &mdash; click to watch video</em>
</p>

## Hardware

| | |
|---|---|
| **Board** | [Waveshare ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/product/arduino/boards-kits/esp32-p4/esp32-p4-wifi6-touch-lcd-4b.htm?sku=31416) |
| **Processor** | Dual-core ESP32-P4 (RISC-V) + ESP32-C6 co-processor for Wi-Fi 6 and BLE |
| **Display** | 4-inch 720x720 IPS, 24-bit color, dimmable backlight |
| **Touch** | GT911 5-point capacitive touchscreen |
| **Memory** | 32 MB PSRAM, 32 MB flash |
| **Storage** | microSD card (4-bit SDMMC) |
| **Connectivity** | Wi-Fi 6, Bluetooth LE |
| **Power** | USB-C (no battery) |
| **Framework** | ESP-IDF v5.5 |

<p align="center">
  <img src="images/ESP32-P4-WIFI6-Touch-LCD-4B-details-size.jpg" alt="Board dimensions and layout" width="100%">
</p>

## Under the Hood

p3a is a C/C++ firmware project built on Espressif's ESP-IDF. The codebase is organized as a set of ESP-IDF components:

| Layer | Components |
|---|---|
| **Core** | `p3a_core` (state machine), `config_store` (NVS settings), `event_bus` |
| **Playback** | `play_scheduler`, `playback_queue`, `channel_manager` |
| **Decoders** | `animation_decoder` (WebP/PNG/JPEG), `animated_gif_decoder` (GIF), `libwebp_decoder` |
| **Connectivity** | `wifi_manager` (provisioning, captive portal, mDNS), `http_api` (REST + WebSocket), `makapix` (MQTT/TLS) |
| **Content** | `giphy` (API + SD caching), `content_cache`, `loader_service`, `storage_eviction` |
| **System** | `ota_manager`, `slave_ota` (C6 firmware), `p3a_board_ep44b` (HAL), `sdio_bus` |
| **Extras** | `pico8` (game streaming), `show_url` (URL artwork download), `ugfx` (graphics primitives) |

The main application code lives in `main/` — boot sequence, display renderer (triple-buffered with VSYNC), animation player pipeline, and playback controller.

Flash is partitioned into dual OTA slots (8 MB each), 4 MB LittleFS for the web UI, 2 MB for ESP32-C6 firmware, plus NVS for configuration. Artwork is stored on the microSD card in a SHA256-sharded vault.

For the full architecture, see [INFRASTRUCTURE.md](docs/INFRASTRUCTURE.md).

## Documentation

| | |
|---|---|
| [HOW-TO-USE.md](docs/HOW-TO-USE.md) | Setup, artwork preparation, touch controls, web UI, REST API, USB, Giphy, Makapix, PICO-8 |
| [flash-p3a.md](docs/flash-p3a.md) | Flashing guide &mdash; web flasher, Windows flasher, command-line methods |
| [INFRASTRUCTURE.md](docs/INFRASTRUCTURE.md) | Architecture, components, build system, display pipeline, networking |

## Contributing

Contributions are welcome. By contributing, you agree that your work will be licensed under the Apache License 2.0.

See [INFRASTRUCTURE.md](docs/INFRASTRUCTURE.md) for technical details about the codebase.

## License

Apache License 2.0. See [LICENSE](LICENSE) for the full text and [LICENSING.md](LICENSING.md) for dependency licenses.

## Acknowledgements

Thanks to ByteWelder from the *Espressif MCUs* Discord for advice early in the project.
