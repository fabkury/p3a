# p3a Quick Start

Get your p3a from box to pixel art on the screen in about 15 minutes.

This guide covers only the steps you need to see something on the display. For everything else — touch gestures, the web dashboard, screen rotation, the REST API, sending art from Makapix Club — see the full [User Guide](HOW-TO-USE.md).

---

## What you need

- **[Waveshare ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/product/arduino/boards-kits/esp32-p4/esp32-p4-wifi6-touch-lcd-4b.htm?sku=31416)** board
- **[microSD card](https://www.waveshare.com/micro-sd-card-16gb-kawau.htm?sku=18191)** (any size; 16 GB is plenty)
- **USB-C cable** that supports data (a charging-only cable won't work)
- **Small screwdriver**
- A computer or phone with Wi-Fi

---

## 1. Insert the microSD card

The card slot is hidden behind the back plate of the board.

1. Unscrew the 4 small screws on the back of the board.
2. Remove the back plate.
3. Notice the microSD card slot on one of the corners. You need to slide to open the metal mechanism, lay the microSD card onto the slot, then slide to close the metal lock.
4. Put the back plate back on and tighten the screws.

The card is where your p3a stores the artwork it downloads and any files you copy onto it.

---

## 2. Flash the firmware

"Flashing" just means installing the p3a software onto the board. The easiest way takes a couple of minutes and only needs a Chrome or Edge browser:

1. Plug the board into your computer with the USB-C cable. Use the **UART** port: it's the port away from the buttons, not the one closer to the buttons.
2. Open the **[p3a Web Flasher](https://fabkury.github.io/p3a/web-flasher/)** in Chrome or Edge.
3. Pick the latest version from the dropdown and click **Connect**. A browser pop-up asks which device to connect to — pick the one that appears.
4. Click **Flash Device** and wait about two minutes. The board reboots when it's done.

Don't have Chrome or Edge, or want an offline Windows app? See the alternatives in [flash-p3a.md](flash-p3a.md).

---

## 3. Connect the device to your Wi-Fi

The first time the board boots, it doesn't know your Wi-Fi yet, so it broadcasts its own temporary network for setup:

1. On your phone or laptop, open the Wi-Fi settings and join the network called **`p3a-setup`**. If the device asks if you want to stay connected despite no internet, click yes.
2. Open a web browser and go to `http://p3a.local/` or `http://192.168.4.1/`.
3. Enter your home Wi-Fi name and password, then click **Save & connect**.
4. The board reboots and joins your Wi-Fi.

That's the technical setup done. Your p3a is now online.

---

## 4. Watch art appear — no account needed

As soon as the board is online, it starts cycling through **Promoted Artworks from Makapix Club** — a curated stream of animated pixel art from real artists. It's free and works without registering for anything.

A new artwork appears roughly every 30 seconds. Try a few touch gestures:

- **Tap the right side** of the screen to skip to the next artwork.
- **Tap the left side** to go back to the previous one.
- **Press and hold** anywhere to see the device's IP address and Wi-Fi info.

If you're happy with this, you're done. Set it on a shelf and enjoy.

---

## Want more art? Pick any of these.

### A. Register at Makapix Club (recommended)

Free, and gives you the entire Makapix Club library plus the ability to send any artwork directly to your device from your phone or laptop:

1. Open `http://p3a.local/settings.html` in any browser on the same Wi-Fi.
2. Go to the **Makapix** tab and click **Enter Provisioning Mode**.
3. A 6-character code appears on the device's screen.
4. Visit **[makapix.club](https://makapix.club/)**, create an account, and paste the code to link your device.

That's it. Now any artwork you tap *Send to p3a* on shows up instantly on the screen.

### B. Show your own GIFs and images

Got a favorite GIF? Drop it onto the dashboard:

1. Open `http://p3a.local/` in any browser on the same Wi-Fi.
2. Drag a `.webp`, `.gif`, `.png`, or `.jpeg` file onto the page.

The file is saved to the SD card and joins the rotation. You can also copy lots of files at once over USB — see the [User Guide](HOW-TO-USE.md#preparing-artwork) for that.

### C. Add trending GIFs from Giphy

Pulls fresh trending GIFs every few hours. This one takes a few extra minutes because Giphy needs a free developer API key:

1. Go to **[developers.giphy.com](https://developers.giphy.com/)**, sign up, and click **Create an API Key**. Give it any name.
2. Copy the **API Key** that gets generated.
3. Open `http://p3a.local/settings.html#giphy` in your browser, paste the key, and click **Save All Giphy Settings**.

Trending GIFs start appearing in the rotation within a few minutes.

---

## Where to next

- **[User Guide](HOW-TO-USE.md)** — touch gestures, web dashboard, REST API, screen rotation, wireless updates, PICO-8 streaming, and the rest.
- **[Flashing alternatives](flash-p3a.md)** — Windows desktop app, command line, building from source.
- **[Makapix Club Discord](https://discord.gg/xk9umcujXV)** — questions, tips, and pixel art chat.
