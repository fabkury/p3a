# p3a — Physical Pixel Art Player

p3a is a Wi-Fi pixel art player that connects to [Makapix Club](https://makapix.club/), a pixel art social network. Play individual artworks or entire channels (like "Promoted Artworks" or "Recent Artworks") directly from the website to your device. Built on the ESP32-P4, it's a smart art frame you can control from anywhere—via touchscreen, web browser, REST API, or securely through Makapix Club's MQTTS backend. Register your p3a at [dev.makapix.club](https://dev.makapix.club/) for cloud connectivity.

## Hardware photos
<p>
  <img src="images/p3a-1.jpg" alt="P3A front" height="320">
  <img src="images/p3a-2.jpg" alt="P3A angled" height="320">
</p>

## Quick start

1. **Get the hardware**: [Waveshare ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/product/arduino/boards-kits/esp32-p4/esp32-p4-wifi6-touch-lcd-4b.htm?sku=31416) + microSD card
2. **Flash the firmware**: Follow the [flashing guide](docs/flash-p3a.md) (5 min, requires Python + USB cable)
3. **Add artwork**: Copy WebP/GIF/PNG/JPEG files to an `animations` folder on the microSD card
4. **Connect to Wi-Fi**: On first boot, connect to `p3a-setup` network and configure your Wi-Fi
5. **Control it**: Open `http://p3a.local/` on your phone or tap the touchscreen

> **Note:** The initial flash requires a command-line tool. After that, all updates are wireless via the web UI.

For detailed usage instructions, see [HOW-TO-USE.md](docs/HOW-TO-USE.md).

## Features

### Makapix Club Integration

- **Play artworks and channels**: Stream individual artworks or entire channels (e.g., "Promoted Artworks", "Recent Artworks") directly from [Makapix Club](https://makapix.club/) to your p3a
- **4 ways to control**: Touchscreen, local web UI at `http://p3a.local/`, REST API, or remotely via Makapix Club's secure MQTTS (MQTT over TLS) backend
- **Cloud connectivity**: Register your device at [dev.makapix.club](https://dev.makapix.club/) to unlock remote control from anywhere
- **Coming soon**: Send "likes" to artworks with a long-press, swipe up to view artwork comments from the Makapix community

### Seamless Playback

- **Freeze-free, gapless playback**: Robust multi-buffer rendering handles animations smoothly, even with corrupt or problematic files
- **Full format support**: Animated WebP and GIF, still PNG and JPEG—all with complete transparency and alpha channel support
- **Aspect ratio preservation**: Non-square artworks display without distortion, properly centered with configurable background colors

### Control & Customization

- **Touch controls**: Tap to change artwork, swipe to adjust brightness, rotate with two fingers
- **Screen rotation**: Rotate the display 0°, 90°, 180°, or 270° via touch gesture or web API
- **Web interface**: Full device control from any browser at `http://p3a.local/`
- **Over-the-Air updates**: After initial USB flash, update firmware wirelessly via the web UI with automatic rollback protection

### Hardware & Connectivity

- **USB access**: Connect via USB-C to access the microSD card as a storage device
- **Auto co-processor updates**: p3a automatically flashes the ESP32-C6 Wi-Fi 6 co-processor firmware when needed
- **PICO-8 Monitor** (optional): Stream PICO-8 games to the display—disabled by default to reduce firmware size, can be enabled at compile time

<p align="center">
  <img src="images/p3a_10fps.gif" alt="p3a video">
</p>

## Over-the-Air Updates

After the initial firmware flash via USB-C cable, all subsequent updates can be installed wirelessly. p3a automatically checks for firmware updates from GitHub Releases, and you can install them directly from the web interface.

**Update features:**
- **Wireless updates** — no need to reconnect USB after the first flash
- **Automatic checks** every 2 hours (or check manually via web UI)
- **One-click install** from the web interface at `http://p3a.local/ota`
- **Progress display** on both the device screen and web interface
- **Automatic rollback** if the new firmware fails to boot 3 times
- **Manual rollback** to previous version via web UI
- **SHA256 verification** ensures firmware integrity
- **ESP32-C6 auto-update** — the Wi-Fi co-processor firmware is updated automatically when needed

<p align="center">
  <a href="images/PXL_20251206_184110573_red.mp4">
    <img src="images/ota_updates.png" alt="OTA update web interface" width="400">
  </a>
  <br>
  <em>OTA update page (click image to watch video)</em>
</p>

## Hardware specs

| Component | Details |
|-----------|---------|
| **Board** | [Waveshare ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/product/arduino/boards-kits/esp32-p4/esp32-p4-wifi6-touch-lcd-4b.htm?sku=31416) |
| **MCU** | Dual-core ESP32-P4 + ESP32-C6 for Wi-Fi 6/BLE |
| **Display** | 4" square 720×720 IPS, 24-bit color, dimmable backlight |
| **Touch** | 5-point capacitive touchscreen |
| **Memory** | 32MB PSRAM, 32MB flash |
| **Storage** | microSD card slot |
| **Power** | USB-C (no battery) |

<p align="center">
  <img src="images/ESP32-P4-WIFI6-Touch-LCD-4B-details-size.jpg" alt="p3a size" width="100%">
</p>

## Current status

p3a is in active development with **nearly complete Makapix Club support**. The following features are implemented:

- Display pipeline with multi-buffer rendering for freeze-free playback
- Animation playback with prefetching and robust error handling
- **Transparency support** for WebP, GIF, and PNG with configurable background color
- **Aspect ratio preservation** for non-square artworks
- Touch gestures (tap, swipe, long-press, two-finger rotation)
- Screen rotation (0°, 90°, 180°, 270°) with persistence
- Wi-Fi provisioning with captive portal
- Local web UI and REST API
- **Makapix Club integration** — play individual artworks and entire channels from [dev.makapix.club](https://dev.makapix.club/)
- **Secure MQTTS client** with device registration and remote control
- **Over-the-Air (OTA) updates** — install firmware updates wirelessly via web UI
- **Automatic ESP32-C6 firmware updates** for the Wi-Fi co-processor
- USB composite device (serial console + mass storage)

**Coming soon:**
- Send "likes" to artworks with long-press gesture
- Swipe up to view artwork comments from the Makapix community

See [ROADMAP.md](docs/ROADMAP.md) for the full development plan.

## Documentation

| Document | Description |
|----------|-------------|
| [HOW-TO-USE.md](docs/HOW-TO-USE.md) | Detailed usage instructions |
| [flash-p3a.md](docs/flash-p3a.md) | How to flash the firmware |
| [OTA_IMPLEMENTATION_PLAN.md](docs/OTA_IMPLEMENTATION_PLAN.md) | Over-the-Air update system design |
| [INFRASTRUCTURE.md](docs/INFRASTRUCTURE.md) | Technical architecture for developers |
| [ROADMAP.md](docs/ROADMAP.md) | Development roadmap and milestones |

## Repository layout

- `main/` — Application entry point, display/touch drivers, animation player, format decoders
- `components/` — Custom components: app state, config store, HTTP API, Makapix integration, OTA manager, decoders
- `managed_components/` — ESP-IDF Component Registry dependencies
- `webui/` — Web interface files (compiled into SPIFFS)
- `docs/` — Documentation

## Makapix Club integration

[Makapix Club](https://makapix.club/) is a pixel-art social network where artists share their creations. Register your p3a at [dev.makapix.club](https://dev.makapix.club/) to:

- **Play artworks and channels**: Stream individual artworks or entire channels (like "Promoted Artworks" or "Recent Artworks") directly to your p3a
- **Remote control**: Control your device from anywhere via secure MQTTS (MQTT over TLS) connection
- **Real-time updates**: Receive artwork notifications instantly
- **Coming soon**: Send "likes" to artworks with a long-press, swipe up to view community comments

## Contributing

Contributions are welcome! See the [INFRASTRUCTURE.md](docs/INFRASTRUCTURE.md) for technical details about the codebase.
