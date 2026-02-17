# p3a — Turn a Touchscreen Dev Board Into a Wi-Fi Pixel Art Player (No Soldering, No Coding)

## Introduction

Ever wanted a tiny animated art frame on your desk that plays trending GIFs, pixel art from an online community, or your own creations — and you can control it from your phone?

p3a turns the Waveshare ESP32-P4-WIFI6-Touch-LCD-4B board into exactly that. It's a 4-inch, 720×720 pixel art player with Wi-Fi, touch gestures, a web interface, over-the-air updates, and integration with both [Giphy](https://giphy.com/) and [Makapix Club](https://makapix.club/) (a pixel art social network).

Here's the thing that makes this project unusual for Instructables: **there is virtually no building involved.** No soldering. No wiring. No 3D-printed enclosure to fiddle with. You buy the board, insert a microSD card, flash the firmware from your browser, and you're done. The whole setup takes about 15 minutes.

The board itself already comes with the display, touchscreen, Wi-Fi 6 (via an ESP32-C6 co-processor), USB-C, and a microSD slot — all in a compact package smaller than a deck of cards. p3a is open-source firmware that brings it all together into a polished, self-updating art player.

**What you'll have when you're done:**

* A desk-sized animated art frame playing GIFs, WebP, PNG, and JPEG files
* Trending Giphy GIFs cycling automatically throughout the day
* A web dashboard at `http://p3a.local/` to control everything from your browser
* Touch controls for skipping artwork, adjusting brightness, and rotating the screen
* Wireless firmware updates — after the first flash, you never need the USB cable again
* Optional cloud features via Makapix Club for remote control and community artwork

<!-- Photo: p3a displaying pixel art on a desk -->

<!-- Reference: images/p3a-1.jpg, images/p3a-2.jpg -->

---

## Step 1: Gather Your Supplies

This is a short list.

### What You Need

|Item|Notes|
|-|-|
|**Waveshare ESP32-P4-WIFI6-Touch-LCD-4B**|The board itself. [Buy from Waveshare](https://www.waveshare.com/product/arduino/boards-kits/esp32-p4/esp32-p4-wifi6-touch-lcd-4b.htm?sku=31416). Also available on Amazon and AliExpress.|
|**microSD card**|Any size works. Even a tiny 2GB card is more than enough.|
|**USB-C data cable**|You probably have one already. Make sure it's a data cable, not a charge-only cable.|
|**Small Phillips screwdriver**|To unscrew the back plate for microSD access.|
|**A computer with Chrome or Edge**|For the one-time firmware flash. Windows, macOS, or Linux.|

That's it. No breadboard, no jumper wires, no resistors, no extra modules.

### About the Board

The Waveshare ESP32-P4-WIFI6-Touch-LCD-4B packs a lot into a small footprint:

* **Processor:** Dual-core ESP32-P4 (RISC-V, 400MHz) — this is Espressif's newest and most powerful chip
* **Wi-Fi:** ESP32-C6 co-processor with Wi-Fi 6 and Bluetooth LE
* **Display:** 4-inch square IPS LCD, 720×720 resolution, 24-bit color
* **Touch:** 5-point capacitive touchscreen
* **Memory:** 32MB PSRAM + 32MB flash
* **Storage:** microSD card slot (SDMMC interface)
* **Connectivity:** 2× USB-C ports
* **Power:** USB-C (no battery)

It's about 72mm × 72mm — roughly the size of a large Post-it note.

<!-- Photo: Board front and back, with microSD slot visible -->

<!-- Reference: images/ESP32-P4-WIFI6-Touch-LCD-4B-details-size.jpg -->

---

## Step 2: Insert the MicroSD Card

The microSD slot is on the back of the board, under the back plate.

1. **Flip the board over** and locate the four small Phillips screws at the corners of the back plate.
2. **Unscrew all four screws** and set them aside. They're small — a magnetic screwdriver helps here.
3. **Carefully lift the back plate.** You'll see the microSD card slot on the PCB.
4. **Insert your microSD card** into the slot.
5. **Replace the back plate** and screw it back down.

The microSD card is where p3a stores cached artwork from Giphy and Makapix, and where you can place your own image files. You don't need to format it or put any files on it right now — p3a will create the directory structure it needs on first boot.

> \*\*Tip:\*\* If you want to pre-load your own artwork, create a folder called `p3a/animations` on the card and drop in any WebP, GIF, PNG, or JPEG files. p3a will find and play them automatically.

---

## Step 3: Flash the Firmware

This is the only step that requires a computer, and it's a one-time thing. After this, all future updates happen wirelessly.

### Using the Web Flasher (Recommended)

The p3a Web Flasher runs entirely in your browser — no software to install.

1. **Open the** [**p3a Web Flasher**](https://fabkury.github.io/p3a/web-flasher/) in Chrome or Edge (Firefox and Safari don't support WebSerial).
2. **Connect the board** to your computer with a USB-C data cable. Use the **Full-Speed (FS)** USB port on the board — it's the one you need for flashing. It is the port farther from the two buttons.
3. **Select the firmware version** from the dropdown (choose the latest).
4. **Click "Connect"** and pick your device from the browser's serial port popup.
5. **Click "Flash Device"** and wait. The progress bar fills up over about 2 minutes.
6. **Done.** The board reboots automatically and the p3a splash screen appears.

That's it — no drivers, no terminal commands, no configuration files.

### Alternative Flashing Methods

If you can't use the web flasher, there are two other options:

* **Windows standalone flasher:** Download `p3a-flasher.exe` from the [GitHub releases](https://github.com/fabkury/p3a/releases). Run it, click "Flash Device," wait 4 minutes. No installation needed.
* **Command line (all platforms):** Install Python and esptool, then run a single command. See the [full flashing guide](https://github.com/fabkury/p3a/blob/main/docs/flash-p3a.md) for details.

### Troubleshooting the Flash

|Problem|Fix|
|-|-|
|"No serial port found"|Try a different USB cable. Many cables are charge-only and don't carry data.|
|Permission denied (Linux)|Run `sudo usermod -a -G dialout $USER`, then log out and back in.|

<!-- Screenshot: Web flasher in browser -->

---

## Step 4: Connect to Wi-Fi

When the board boots up for the first time, it doesn't know your Wi-Fi network yet, so it creates its own.

1. **On your phone or computer**, open your Wi-Fi settings and connect to the network called **`p3a-setup`**.
2. **A captive portal page should open automatically.** If it doesn't, open your browser and go to `http://p3a.local/` or `http://192.168.4.1`.
3. **Enter your home Wi-Fi network name (SSID) and password.**
4. **Click "Save \& Connect."**
5. **The board reboots** and connects to your Wi-Fi network. The `p3a-setup` network disappears.

From now on, you can access your p3a at **`http://p3a.local/`** from any device on the same network. This is the address of the built-in web dashboard where you can control everything.

> \*\*Note:\*\* If `p3a.local` doesn't work on your network (some corporate and guest networks block mDNS), check your router's DHCP client list for the device's IP address and use that instead.

---

## Step 5: Set Up Giphy (Optional but Fun)

One of p3a's best features is that it can pull trending GIFs directly from Giphy and cycle through them all day. Setting this up takes about 2 minutes.

### Get a Free Giphy API Key

1. Go to [developers.giphy.com](https://developers.giphy.com/) and create an account (it's free).
2. Click **Create an App** and choose the **API** option.
3. Give it any name (e.g., "p3a") and description.
4. Copy the **API Key** it gives you.

### Enter the Key in p3a

1. Open **`http://p3a.local/giphy.html`** in your browser.
2. Paste your API key into the API Key field.
3. Optionally adjust the settings:

   * **Rendition:** "Fixed Height" is a good default. "Original" gives the highest quality but some animations become slower than intended.
   * **Format:** WebP looks better. GIF plays faster.
   * **Content Rating:** G, PG, PG-13, or R — pick your comfort level.
   * **Refresh Interval:** How often to fetch new trending GIFs (default is fine).
   * **Cache Size:** How many GIFs to keep on the SD card at a time.

4. Click **Save All Settings**.
5. Go back to `**http://p3a.local/` and click on the "Giphy Trending" button.**

Within a few seconds, p3a starts downloading trending GIFs and playing them. The first few may take a moment to load as they're downloaded, but once cached, playback is instant.

<!-- Photo: p3a displaying a Giphy GIF -->

<!-- Reference: images/p3a-3-giphy.jpg, images/p3a-4-giphy.jpg -->

---

## Step 6: Learn the Touch Controls

The entire 720×720 touchscreen is interactive. Here are the gestures:

|Gesture|What It Does|
|-|-|
|**Tap the right half**|Skip to the next artwork|
|**Tap the left half**|Go back to the previous artwork|
|**Swipe up**|Increase brightness|
|**Swipe down**|Decrease brightness|
|**Two-finger rotate**|Rotate the screen 90° (clockwise or counter-clockwise)|
|**Long press**|Start device registration for Makapix Club|

### Screen Rotation

This is one of those small touches that makes a big difference. Place two fingers on the screen and rotate them like turning a dial. When you pass about 45°, the screen snaps to the next 90° orientation. It remembers your choice across reboots, so you can mount the device in any orientation — landscape, portrait, upside down — whatever works for your desk or shelf.

You can also set the rotation from the web interface or REST API if you prefer.

### Auto-Advance

When you're not touching the screen, p3a automatically advances to a new artwork every 30 seconds. This can be changed. Just set it up and let it cycle.

---

## Step 7: Explore the Web Dashboard

Open **`http://p3a.local/`** in any browser on your local network. This is your p3a command center.

The web interface gives you:

* **Device status** — what's currently playing, Wi-Fi signal strength, uptime, memory usage
* **Playback controls** — Next, Previous, Pause, Resume buttons
* **Brightness slider** — adjust without touching the device
* **Rotation control** — set the display orientation
* **Background color picker** — choose what color shows behind transparent artwork
* **Settings page** — Wi-Fi configuration, storage info, root directory settings
* **Giphy settings** — API key, content rating, format, cache size
* **OTA updates** — check for and install firmware updates wirelessly

The web UI is only accessible on your local network, not from the internet. If you want remote control, that's where Makapix Club comes in (next step).

<!-- Screenshot: p3a web dashboard -->

---

## Step 8: Register With Makapix Club (Optional)

[Makapix Club](https://makapix.club/) is a pixel art social network. Registering your p3a there unlocks some powerful features:

* **Browse artwork online** and send it directly to your device with one click
* **Play entire channels** — curated collections like "Promoted Artworks" or "Recent Artworks"
* **Remote control** — skip artwork, change artwork, pause/resume from anywhere with an internet connection
* **Mix sources** — combine Makapix channels with Giphy channels for a varied, ever-changing display

### How to Register

1. **Long-press the touchscreen** on your p3a. A 6-character registration code appears on the display.
2. **Go to** [**makapix.club**](https://makapix.club/) on your phone or computer.
3. **Create an account** (or log in if you already have one).
4. **Enter the registration code** to link the device to your account.

That's it. The device connects securely via TLS-encrypted MQTT and you can start sending artwork to it immediately.

> \*\*Security note:\*\* The connection uses mutual TLS (mTLS) with unique client certificates for each device. Your device gets its own cryptographic identity during registration.

The registration code expires after 15 minutes. If it times out, just long-press the screen again to get a fresh one.

**Join the community:** The [Makapix Club Discord](https://discord.gg/xk9umcujXV) is where p3a users hang out, share tips, and discuss pixel art.

---

## Step 9: Add Your Own Artwork

p3a plays four image formats: **animated WebP**, **animated GIF**, **PNG** (with alpha channel), and **JPEG**. You can load your own files in two ways.

### Option A: Copy via USB

1. **Connect a USB-C cable** from the **High-Speed (HS)** USB port on the board to your computer. (The board has two USB-C ports — use the HS one. It is the one closer to the two buttons.)
2. **The microSD card appears as a removable drive** on your computer.
3. **Navigate to the `p3a/animations/` folder** (p3a creates this automatically).
4. **Drop in your image files.** Any WebP, GIF, PNG, or JPEG will work.
5. **Eject the drive** and disconnect the cable.

### Option B: Pre-Load the SD Card

1. **Remove the microSD card** (unscrew the back plate).
2. **Insert it into your computer** using a card reader.
3. **Create a folder called `animations`** and copy your files into it.
4. **Reinsert the card** into the board.

### Tips for Great-Looking Artwork

* **Any aspect ratio works.** Non-square images are scaled to fit the 720×720 display while preserving their proportions, centered on the screen.
* **Pixel art looks fantastic** because p3a uses nearest-neighbor scaling, which keeps those crisp pixel edges sharp instead of blurring them.
* **Transparency is fully supported** in WebP, GIF, and PNG. Transparent areas show the background color, which you can change from the web interface.
* **WebP is the recommended format** — it gives the best quality at the smallest file size, especially for animations.
* GIF provides the fastest decoding — large animations may become slow if using animated WebP.

<!-- Photo: p3a playing pixel art -->

<!-- Reference: images/p3a\_10fps.gif -->

---

## Step 10: Wireless Updates — Set It and Forget It

Here's something genuinely convenient: after the initial USB flash, **you never need to plug in a cable again.** p3a checks for firmware updates automatically and lets you install them from the web interface.

### How OTA Updates Work

* p3a checks GitHub for new releases every 2 hours.
* Updates are **never installed automatically** — you always approve them manually.
* When an update is available, a notification appears on the web dashboard.

### Installing an Update

1. Open **`http://p3a.local/ota`** in your browser.
2. If an update is available, you'll see the new version number and release notes.
3. Click **"Install Update"** and confirm.
4. Watch the progress bar on both the web page and the device screen.
5. The device reboots automatically when the update finishes.

### Rollback Protection

If something goes wrong with an update, p3a has your back:

* **Automatic rollback:** If the new firmware fails to boot 3 times in a row, it automatically reverts to the previous working version.
* **Manual rollback:** Visit `http://p3a.local/ota` and click "Rollback to Previous" at any time.

The ESP32-C6 Wi-Fi co-processor firmware is also updated automatically when needed — you don't have to think about it.

<!-- Screenshot: OTA update page -->

<!-- Reference: images/ota\_updates.png -->

---

## Step 11: Bonus Features

### REST API for Automation

p3a exposes a JSON REST API at the same address as the web interface. If you're into home automation or scripting, you can control everything programmatically:

```bash
# Get device status
curl http://p3a.local/status

# Skip to next artwork
curl -X POST http://p3a.local/action/swap\_next

# Pause playback
curl -X POST http://p3a.local/action/pause

# Set screen rotation to 90 degrees
curl -X POST http://p3a.local/api/rotation \\
  -H "Content-Type: application/json" \\
  -d '{"rotation": 90}'

# List files on the SD card
curl "http://p3a.local/files/list?path=/sdcard/p3a/animations"
```

This makes it straightforward to integrate p3a with Home Assistant, Node-RED, or your own scripts.

### PICO-8 Game Streaming

If you're a [PICO-8](https://www.lexaloffle.com/pico-8.php) fan, p3a can act as a wireless game display. Open the web interface, click the PICO-8 button, load a `.p8` cartridge, and the game streams to the device's screen at 30 FPS via WebSocket. The 720×720 display gives PICO-8's 128×128 output a gorgeous, pixel-perfect 5.6× upscale.

<!-- Reference: images/pico-8-gameplay.webp -->

---

## Step 12: Troubleshooting and Tips

### Common Issues

|Problem|Solution|
|-|-|
|`p3a.local` doesn't resolve|Some networks block mDNS. Check your router for the device's IP address and use that instead.|
|Touch not responding|Reboot by pressing the RESET button.|
|Can't connect to Wi-Fi|Double-check the password. p3a supports WPA2 and WPA3. If the captive portal doesn't appear, browse to `http://192.168.4.1` or `http://p3a.local/` manually.|
|Giphy GIFs not loading|Verify your API key at `http://p3a.local/giphy.html`.|
|Device behaving unexpectedly|Try changing the **root directory** in the web UI settings. The default is `/p3a`. Switching to a new directory creates fresh index files, which can resolve issues from corrupted cache data. You can always switch back.|

### Useful Links

* **GitHub repository:** [github.com/fabkury/p3a](https://github.com/fabkury/p3a) — source code, releases, and issue tracker
* **Flashing guide:** [flash-p3a.md](https://github.com/fabkury/p3a/blob/main/docs/flash-p3a.md) — detailed instructions for all three flashing methods
* **Full usage guide:** [HOW-TO-USE.md](https://github.com/fabkury/p3a/blob/main/docs/HOW-TO-USE.md) — everything about touch controls, web UI, REST API, and more
* **Makapix Club:** [makapix.club](https://makapix.club/) — pixel art community and remote device control
* **Discord:** [Makapix Club Discord](https://discord.gg/xk9umcujXV) — community chat for p3a users
* **Waveshare product page:** [ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/product/arduino/boards-kits/esp32-p4/esp32-p4-wifi6-touch-lcd-4b.htm?sku=31416)

---

## Final Thoughts

What I like about this project is what it *doesn't* require. No custom PCB. No enclosure to print. No wiring diagrams to follow. The hardware comes fully assembled with a beautiful display, and the firmware does the rest.

Once it's running, p3a is genuinely a "set it and forget it" device. Giphy keeps the content fresh automatically. Firmware updates happen over Wi-Fi. The touchscreen lets you skip, dim, or rotate without reaching for your phone. And if you get into the Makapix Club community, you've got an endless stream of pixel art from real artists landing on your desk. Tired of it? Press the pause button on http://p3a.local/ and the screen turns off. Unpause, and the animations are back.

It's a fun weekend project that stays useful long after the weekend is over.

p3a is open source under the Apache 2.0 license. Contributions, bug reports, and feature requests are welcome on [GitHub](https://github.com/fabkury/p3a).

