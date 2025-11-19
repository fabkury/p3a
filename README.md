# p3a (Pixel Pea) — Physical Pixel Art Player

p3a ("Pixel Pea") is a physical pixel art player inside the Makapix Club ecosystem. It is an ESP32-P4-powered Wi-Fi art frame that subscribes to online feeds, downloads and displays pixel artworks, and lets viewers react (send likes) and read comments without leaving the hardware experience. The device has touchscreen controls, and the firmware also serves a browser interface and REST API at [http://p3a.local/](http://p3a.local/) for control using a phone, laptop, or code. Plus, it also doubles as a little PICO-8 game monitor.

## Hardware photos
<p>
  <img src="images/p3a-1.jpg" alt="P3A front" height="320">
  <img src="images/p3a-2.jpg" alt="P3A angled" height="320">
</p>

## How it feels like to use it
Set p3a on your shelf and it becomes a quiet pixel art gallery that keeps moving on its own. Playback is ordered or random, you choose.
- Tap the screen to jump to the next or previous artwork.
- Swipe up or down to adjust brightness.
- Long-tap to send a like to the current artwork.
- Open `http://p3a.local/` on your phone to control the device.
- Connect a USB-C cable to a computer or smartphone to access all files in the device's microSD card.
- 🎮 Plus: use p3a as a PICO-8 game monitor.

## *What if I don't know about coding or firmwares?*
Flashing the prebuilt firmware is straightforward using the steps below, but I know that can still feel intimidating if you are not into programming or microcontrollers. I am working on a browser-based flasher. That means you’ll be able to visit one website, plug in your device to your computer (or phone) using a regular USB-C cable and click a button. No command lines required, no need to create an user account to flash. Bookmark this page and check back later for that update.

## Hardware platform & specs
The entire hardware platform consists of one device, [ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/product/arduino/boards-kits/esp32-p4/esp32-p4-wifi6-touch-lcd-4b.htm?sku=31416) ("EP44B"), plus one microSD card. EP44B comes finished out of the box, there is no physical assembly beyond inserting a microSD card in its slot. After that, the unit is ready to connect via USB-C cable and flash (install) the p3a firmware from this repository.
- **Board**: [Waveshare ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/product/arduino/boards-kits/esp32-p4/esp32-p4-wifi6-touch-lcd-4b.htm?sku=31416) ("EP44B")
  - dual-core ESP32-P4 host MCU
  - onboard ESP32-C6 for Wi-Fi 6/BLE
  - 32MB external PSRAM, 32MB NOR flash
  - GPIO expansion, onboard red LED, provision for speakers/mics per BSP
- **Display**: 4" square 720×720 pixels IPS panel with dimmable backlight, 24-bit color, 400 cd/m² max brightness
- **Touch**: 5-point capacitive touchscreen
- **Storage**: microSD card (not included)
- **Power source**: USB-C cable (not included) (no battery)

<p align="center">
  <img src="images/ESP32-P4-WIFI6-Touch-LCD-4B-details-size.jpg" alt="p3a size" width="100%">
</p>

## Current firmware capabilities
- **Display pipeline**: Initializes the Waveshare LCD, manages multi-buffer swaps, and exposes brightness control through PWM.
- **Animation playback**: Scans the SD card for WebP/GIF/PNG/JPEG files, decodes them on background tasks, and keeps playback smooth with prefetching.
- **Animation library priority**: If `/sdcard/animations` exists it is used immediately; otherwise the player traverses the whole card to find the next best folder automatically.
- **Touch input**: GT911 gestures — tap left/right to swap animations, vertical swipes adjust brightness.
- **Auto rotation & remote control**: Auto-randomizes artworks when idle and accepts touch, REST, and the web UI at `http://p3a.local/` for status, configuration, and manual swaps.
- **Wi-Fi provisioning**: Station mode with captive portal fallback; credentials persist across reboots.
- **Config knobs**: `menuconfig` toggles for asset paths, playback timing, render formats, task priorities, and gesture tuning.
- **USB composite (HS port)**: Presents CDC-ACM, SD Mass Storage, and a vendor bulk pipe for 128×128 PICO-8 streaming at high speed while keeping the ROM bootloader (FS port) unchanged for flashing.

<p align="center">
  <img src="images/p3a_10fps.gif" alt="p3a video">
</p>

## Planned functionality (see ROADMAP.md)
p3a is in the **display prototype with Wi-Fi** stage. Upcoming milestones focus on:

- **Connectivity**: TLS MQTT client, Makapix Club feed ingestion, quick reactions from the hardware.
- **Playlists & UI**: Playlist-aware scheduling, richer gestures, and lightweight HUD overlays.
- **Reliability**: OTA with rollback, watchdog coverage, diagnostics, and provisioning workflows.
- **Manufacturing & docs**: Flashing tools, release automation, and installer tutorials.
- **Extra**: ✅ **PICO-8 Monitor** — Stream games wirelessly from your browser! (See PICO-8 Monitor section)

See `ROADMAP.md` for a detailed phase-by-phase breakdown.

## Build & flash
> **💡 Coming up: Web Flasher!**  
> You'll be able to flash p3a to your device directly from your browser—no downloads, terminals, or drivers required. Stay tuned for updates.

1. Install **ESP-IDF v5.5.x** with the `esp32p4` target (IDF Component Registry dependencies are auto-synced via `idf.py`).
2. In this repo, set the target once: `idf.py set-target esp32p4`.
3. Optionally tweak project options via `idf.py menuconfig` (display format, gesture thresholds, animation path, task priorities).
4. Build & flash: `idf.py build flash monitor`. Use `-p PORT`/`-b BAUD` as needed.

### Flashing prebuilt images (no build required)
1. Install [esptool](https://github.com/espressif/esptool) or use the copy bundled with ESP-IDF.
2. Grab the following files from `build/` (or a release package): `bootloader/bootloader.bin`, `partition_table/partition-table.bin`, `p3a.bin`, plus the helper files `flash_args` and `flasher_args.json`.
3. Connect the board in download mode and run:

```bash
esptool.py --chip esp32p4 --before default_reset --after hard_reset \
  --flash_mode dio --flash_freq 80m --flash_size 32MB write_flash \
  0x2000 bootloader/bootloader.bin \
  0x8000 partition_table/partition-table.bin \
  0x10000 p3a.bin
```

Use `--port` and `--baud` if esptool cannot auto-detect the serial port. The offsets and flash settings are mirrored in `flash_args` for convenience.

### Preparing artwork media
- The firmware **requires** a microSD card inserted into the SDMMC slot.
- Either copy your artwork files to the microSD card before inserting it in p3a, or copy using p3a and a USB-C cable (p3a appears as a storage device).
- Supported containers today: animated/non-animated **WebP, GIF, PNG, JPEG**. Source canvases are upscaled to 720×720 so keep square canvases.
- Drop artworks preferably into `/sdcard/animations`; if that folder is empty the firmware automatically searches the rest of the microSD card for a folder containing at least one animation file.

### Accessing the microSD card with a USB-C cable
p3a has two USB-C ports, but only one works as a USB storage device when connected to a laptop/desktop/smartphone. Moreover, while p3a is acting as a storage device, the microSD card becomes unavailable for the rest of the system, so animations cannot be changed but the currently playing one continues. Normal behaviors resume after disconnecting the storage device.

### 🎮 PICO-8 Monitor — Turn Your Device Into a Retro Game Display
Transform p3a into a dedicated PICO-8 monitor! Stream your favorite PICO-8 games directly from your browser to the device's display in real-time.

**How it works**: Open `http://p3a.local/` in your browser, click the "PICO-8" button. Load any `.p8` or `.p8.png` cart file, and watch as the game streams wirelessly to your device. The browser runs a WebAssembly PICO-8 emulator (fake-08) and sends each frame over WebSocket to p3a, which renders it on the 720×720 display with crisp nearest-neighbor upscaling.

The device automatically enters PICO-8 mode when you connect, and falls back to normal playback after 30 seconds of inactivity or when you close the browser.

<p align="center">
  <img src="images/pico-8-gameplay.webp" alt="PICO-8 gameplay video">
</p>

### On-device controls
- **Tap right half**: advance to the next animation.
- **Tap left half**: go back to the previous animation.
- **Vertical swipe**: adjust brightness proportionally to the swipe distance; swiping up brightens, swiping down dims.
- **Idle auto-swap**: every N seconds (configurable via `CONFIG_P3A_AUTO_SWAP_INTERVAL_SECONDS`, default 30s) the unit picks a random animation unless the user has interacted recently (via touch or REST API).

### Wi-Fi setup
On first boot or if saved credentials fail, the device starts a captive portal:
1. Connect to the Wi-Fi network `p3a-setup` (or the SSID configured via `CONFIG_ESP_AP_SSID`).
2. Open `http://192.168.4.1` in your browser.
3. Enter your Wi-Fi SSID and password, then click "Save & Connect".
4. The device will reboot and connect to your network.

Once connected, open `http://p3a.local/` (mDNS) or use the device's IP address for the web dashboard and REST API.

### Web & REST control
- `http://p3a.local/` serves a browser UI with status, configuration, and swap controls. This website is only accessible in the local Wi-Fi, not over the internet.
- The same endpoints speak JSON for automation. Example `curl` calls:

```bash
# Get device status
curl http://p3a.local/status

# Advance to next animation
curl -X POST http://p3a.local/action/swap_next

# Go back to previous animation
curl -X POST http://p3a.local/action/swap_back

# Reboot device
curl -X POST http://p3a.local/action/reboot
```

## Repository layout
- `main/` — application entry point, LCD/touch wrappers, animation player, format decoders, and Wi-Fi manager.
- `components/` — vendored decoders (animated GIF, libwebp support glue), app state management, config store, and HTTP API.
- `managed_components/` — ESP-IDF Component Registry dependencies (Waveshare BSP, esp_lcd_touch, libpng, etc.).
- `def/` — sdkconfig defaults for the esp32p4 target.
- `ROADMAP.md` — execution plan for each firmware milestone.

## Makapix Club integration primer
Makapix Club (https://makapix.club/) is a pixel-art social network that hosts artworks and offers metadata, moderation, reactions and MQTT notifications. p3a’s planned role is:
- Subscribe to Makapix Club MQTT to fetch the URLs for new artworks/playlists.
- Download media over HTTPS, cache it locally and include in display rotation.
- Allow viewers to send a like and to fetch the likes and comments for the currently displayed post.

Contribute to this project!
