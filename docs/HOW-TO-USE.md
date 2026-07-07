# How to Use p3a

This guide covers everything you need to know to use your p3a pixel art player, from initial setup to advanced features.

> **First time setting up?** Start with the [Quick Start Guide](QUICK-START.md) — it walks through the bare minimum to see art on the screen in about 15 minutes. Come back here for the deeper details.

> **Built to stay on.** p3a's hardware and firmware are designed for continuous 24/7 operation. Once it's set up, you can leave it running on a shelf or wall indefinitely — no scheduled reboots, no daily power cycle, no babysitting required.

## Table of Contents

1. [Initial Setup](#initial-setup)
2. [Preparing Artwork](#preparing-artwork)
3. [Touch Controls](#touch-controls)
4. [Wi-Fi Setup](#wi-fi-setup)
5. [Web Interface](#web-interface)
6. [REST API](#rest-api)
7. [USB SD Card Access](#usb-sd-card-access)
8. [Firmware Updates](#firmware-updates)
9. [Giphy Integration](#giphy-integration)
10. [Klipy Integration](#klipy-integration)
11. [Museum Channels (IIIF)](#museum-channels-iiif)
12. [Device Registration](#device-registration)
13. [Makapix Club Features](#makapix-club-features)
14. [PICO-8 Monitor](#pico-8-monitor-optional)

---

## Initial Setup

### What you need

- Waveshare ESP32-P4-WIFI6-Touch-LCD-4B board
- USB-C data cable (not a charging-only cable)
- microSD card (8 GB minimum; see [SD card sizing](#sd-card-sizing) below)
- a small screwdriver

### First-time setup

1. **Insert the microSD card** into the slot on the board. This requires unscrewing the back plate.
2. **Flash the firmware**. See [flash-p3a.md](flash-p3a.md) for instructions.
3. **Configure Wi-Fi** by following the [Wi-Fi Setup](#wi-fi-setup) instructions.

### SD card sizing

p3a downloads and caches artwork on the SD card, and runs an automatic eviction policy that keeps a guaranteed amount of free space at all times. Under the default settings:

- Eviction kicks in when free space drops below **1 GiB** (the *trigger* watermark).
- It deletes the least-recently-modified cached artwork until free space rises to at least **5 GiB** (the *stop* watermark = trigger + 4 GiB headroom). The 4 GiB overshoot prevents thrashing — without it, a few downloads would push free space back across the trigger line almost immediately.
- Files newer than 4 hours are never deleted.

---

## Preparing Artwork

### Supported formats

p3a supports these image formats:
- **WebP** (animated and static) — recommended for best quality and compression; supports transparency
- **GIF** (animated and static) — supports transparency
- **PNG / APNG** (animated and static) — supports transparency with full alpha channel; `.apng` files are accepted too
- **JPEG** (static)
- **BMP** (static) — all common variants (1/4/8/16/24/32-bit, RLE compression); transparency supported for files with an explicit alpha mask (V4/V5 headers)

**Transparency support**: Images with transparent backgrounds or alpha channels are fully supported. The background color behind transparent areas can be configured via the web interface or REST API.

### File organization

Your own files live inside a single folder on the microSD card. By default that folder is:

```
/p3a/animations/
```

The firmware looks **only** in this folder for local artwork — it does not scan the rest of the card. If the folder is missing or empty, the local channel just shows zero artworks (it does not fall back to other locations).

**Important — the `p3a` folder is configurable.** `p3a` is the device's "root" (or master) folder, and it can be renamed from the web interface at `http://p3a.local/settings.html` → **Storage** tab → *SD Card Storage*. Whatever you set there (e.g. `/p3a`, `/myart`, `/data`) is the folder p3a uses for **all** of its data — local files, Makapix vault, Giphy and Klipy caches, playlists, etc. Changes require a reboot.

Before manually placing files on the SD card, **always check the Storage tab to confirm the current root folder** — if it has been changed from the default, dropping files into `/p3a/animations/` will have no effect. Place them under `<your-root>/animations/` instead.

**Recommended setup:**
1. Open `http://p3a.local/settings.html` → **Storage** tab and note the current SD card path (default: `/p3a`)
2. On the microSD card, create an `animations` subfolder inside that root folder (e.g. `/p3a/animations/`)
3. Copy your artwork files into that `animations` subfolder

You can copy files directly over USB: connect a USB-C cable from the **HS port** to your computer and the SD card appears as a removable drive (see [USB SD Card Access](#usb-sd-card-access) below). You can also upload files through the web interface — uploads are saved into the same `<root>/animations/` folder automatically.

### Image guidelines

- **Any aspect ratio works** — non-square images are scaled to fit while preserving the original aspect ratio
- **Small pixel art is upscaled** — a 128×128 image will be scaled up to fill the display
- **Smart upscaling** — pixel art (Makapix and local files) uses nearest-neighbor scaling to preserve crisp pixel edges, while Giphy, Klipy, and museum content uses hardware-accelerated bilinear interpolation for smooth results
- **Centered display** — artwork is centered on the 720×720 display with configurable background color
- **Transparent artwork** — images with transparency are composited over the configured background color

---

## Touch Controls

The 720×720 touchscreen recognizes these gestures:

| Gesture | Action |
|---------|--------|
| **Tap right half** | Advance to next artwork |
| **Tap left half** | Go back to previous artwork |
| **Swipe up** | Like the current Makapix artwork (or register a Giphy click); pin a Klipy or museum artwork |
| **Swipe down** | Revoke a Makapix like; unpin a Giphy, Klipy, or museum artwork |
| **Two-finger rotate** | Rotate screen (clockwise or counter-clockwise) |
| **Long press** | Show device info / dismiss overlay |

### Screen Rotation

You can rotate the display to any of four orientations (0°, 90°, 180°, 270°) using a two-finger rotation gesture:

1. **Place two fingers on the screen**
2. **Rotate your fingers** clockwise or counter-clockwise (like turning a dial)
3. **When you rotate ~45°**, the screen rotates 90° in the same direction
4. **The rotation persists** across reboots and is saved automatically

The rotation applies to both animation playback and the UI (registration codes, status messages). This is useful for mounting the device in different orientations or viewing artwork from different angles.

You can also set rotation via the web interface or REST API (see [REST API](#rest-api) below).

### Auto-advance

When idle (no touch or API interaction), the device automatically advances to the next artwork roughly every 30 seconds. The next artwork is chosen by the active playset's pick mode (recency or random). The interval is configurable at runtime via the web interface or REST API, with a compile-time default of 30 seconds.

---

## Wi-Fi Setup

### First boot / new network

If the device can't connect to a saved network, it starts a captive portal:

1. **Connect to the Wi-Fi network** `p3a-setup` from your phone or computer
2. **A setup page should open automatically.** If not, open `http://p3a.local/` or `http://192.168.4.1` in your browser
3. **Enter your Wi-Fi credentials** (SSID and password)
4. **Click "Save & connect"**
5. The device reboots and connects to your network

### After connection

Once connected, you can access the device at:
- **mDNS**: `http://p3a.local/` (works on most networks)
- **IP address**: Long-press on the screen to show info screen with the device's IP address

---

## Web Interface

Open `http://p3a.local/` in any browser on the same Wi-Fi network to access the web dashboard.

The web interface provides:
- **Device status** — current artwork (Wi-Fi info and uptime are on the Settings → Network tab)
- **Playback controls** — next, previous, pause, resume
- **Configuration** — brightness, screen rotation, settings
- **PICO-8 button** (if the feature is enabled in firmware)

> **Note:** The web interface is only accessible on your local network, not over the internet. For remote control, register your device at [makapix.club](https://makapix.club/).

---

## REST API

The same endpoints that power the web interface are available as a JSON API for automation and scripting.

### Get device status

```bash
curl http://p3a.local/status
```

Returns JSON with playback state, firmware version, network info, uptime, free heap, and storage. For current artwork details, see `GET /api/init` or `GET /playsets/active`.

### Control playback

```bash
# Advance to next artwork
curl -X POST http://p3a.local/action/swap_next

# Go back to previous artwork
curl -X POST http://p3a.local/action/swap_back

# Pause playback
curl -X POST http://p3a.local/action/pause

# Resume playback
curl -X POST http://p3a.local/action/resume

# Reboot device
curl -X POST http://p3a.local/action/reboot
```

### Screen rotation

```bash
# Get current rotation
curl http://p3a.local/rotation

# Set rotation (0, 90, 180, or 270 degrees)
curl -X POST http://p3a.local/rotation -H "Content-Type: application/json" -d '{"rotation": 90}'
```

### File management

File management is not available via the REST API. To add or remove artwork files, use:

- **USB Mass Storage** (see [USB SD Card Access](#usb-sd-card-access)) — connect via USB-C and the SD card mounts as a removable drive
- **Web UI upload** — drag and drop files onto the dashboard at `http://p3a.local/`

---

## USB SD Card Access

p3a has two USB-C ports, but only one (the High-Speed port) works as a USB storage device.

### To access the microSD card from your computer:

1. **Connect a USB-C cable** from the HS port to your computer
2. **The microSD card appears as a removable drive**
3. **Copy, delete, or organize files** as needed
4. **Eject the drive** before disconnecting

### Important notes

- While connected as USB storage, the SD card is locked for the device
- The screen shows a USB-mode indicator while connected; you can't change artwork until disconnected
- Normal operation resumes after disconnecting the USB cable
- Works with computers, smartphones (with USB OTG), and tablets

---

## Firmware Updates

p3a supports Over-the-Air (OTA) firmware updates. After the initial firmware flash via USB-C cable, all subsequent updates can be installed wirelessly through the web interface.

### How OTA updates work

- **First flash**: Use a USB-C cable to flash the initial firmware (see [flash-p3a.md](flash-p3a.md))
- **Subsequent updates**: Download and install wirelessly via the web UI at `http://p3a.local/ota`
- **ESP32-C6 co-processor**: The Wi-Fi module's firmware is updated automatically when needed

### Automatic update checks

- The device checks for updates every 12 hours while connected to Wi-Fi
- Updates are **never installed automatically**—you always approve updates manually
- A notification appears on the main web page when an update is available

### Installing an update

1. **Open the web interface** at `http://p3a.local/`
2. **Look for the update notification** (green banner) if an update is available
3. **Click the notification** or go to `http://p3a.local/ota`
4. **Review the available version** and release notes
5. **Click "Install Update"** and confirm
6. **Wait for the update to complete**—progress is shown on both the device screen and web interface
7. **The device reboots automatically** when the update is complete

> **Important:** Do not power off the device during an update. The update process takes 1-2 minutes depending on the firmware size.

### Rollback to previous version

If you need to revert to the previous firmware version:

1. Go to `http://p3a.local/ota`
2. If a previous version is available, a **"Rollback"** button will appear
3. Click the rollback button (labeled **"Rollback to <previous version>"**) and confirm
4. The device reboots with the previous firmware

### REST API for updates

```bash
# Check current update status
curl http://p3a.local/ota/status

# Trigger manual update check
curl -X POST http://p3a.local/ota/check

# Install available update (if one exists)
curl -X POST http://p3a.local/ota/install

# Rollback to previous version
curl -X POST http://p3a.local/ota/rollback
```

---

## Giphy Integration

p3a can play trending animated GIFs directly from [Giphy](https://giphy.com/). Once configured, Giphy channels appear alongside your Makapix and local artwork channels, and you can mix them in the same playset.

### Getting a Giphy API key

To use Giphy, you need a free API key:

1. Go to [developers.giphy.com](https://developers.giphy.com/) and sign up
2. Create an app and copy the API key
3. Open the p3a settings page at `http://p3a.local/settings#giphy`
4. Paste your API key and click **Save All Giphy Settings**

### Configuring Giphy

The Giphy tab at `http://p3a.local/settings#giphy` lets you customize:

| Setting | Options | Description |
|---------|---------|-------------|
| **Content Rating** | G, PG, PG-13, R | Filters content by age-appropriateness |
| **Refresh Interval** | 1, 2, 4, or 8 hours | How often p3a fetches fresh trending content from Giphy |
| **Cache Size** | 32–500 items | Maximum number of GIFs kept per Giphy channel |

> Rendition (resolution) and format (WebP/GIF) are build-time defaults set via Kconfig (`CONFIG_GIPHY_RENDITION_DEFAULT`, `CONFIG_GIPHY_FORMAT_DEFAULT`); they are not adjustable from the web UI.

### How it works

- p3a periodically fetches trending GIFs from the Giphy API and caches them on the SD card
- Downloaded artworks are stored in the `giphy/` subfolder of your configured SD card root (default: `/sdcard/p3a/giphy/`) and managed automatically
- The device downloads GIFs on demand as they come up in the rotation, so the first few plays may take a moment to load
- Once cached, playback is instant — just like local files

---

## Klipy Integration

p3a can play GIFs and stickers from [Klipy](https://klipy.com/). Once configured, Klipy channels appear alongside your Makapix, Giphy, and local artwork channels, and you can mix them in the same playset. Klipy offers three channel kinds — trending, search, and category — each available for both GIFs and stickers.

### Getting a Klipy API key

To use Klipy, you need a free API key:

1. Go to [partner.klipy.com](https://partner.klipy.com/) and sign up
2. Copy your app's API key
3. Open the p3a settings page at `http://p3a.local/settings#klipy`
4. Paste your API key and click **Save All Klipy Settings**

> Test keys are limited to 100 requests per hour. Each request returns up to 50 items, so a 500-item channel takes 10 requests to refresh. Production keys are unlimited.

### Configuring Klipy

The Klipy tab at `http://p3a.local/settings#klipy` lets you customize:

| Setting | Options | Description |
|---------|---------|-------------|
| **Content Rating** | G, PG, PG-13, R | Filters content by age-appropriateness |
| **Format** | GIF or WebP | Downloaded file format — GIF by default; WebP has a smaller download footprint |
| **Refresh Interval** | 1, 2, 4, or 8 hours | How often p3a fetches fresh content from Klipy |
| **Cache Size** | 32–2500 items | Maximum number of artworks kept per Klipy channel |

### How it works

- p3a periodically fetches each channel's listing (trending, search, or category) from the Klipy API and caches it on the SD card
- Downloaded artworks are stored in the `klipy/` subfolder of your configured SD card root (default: `/sdcard/p3a/klipy/`), split into `gif/` and `sticker/` subtrees
- The device downloads artworks on demand as they come up in the rotation, so the first few plays may take a moment to load
- Once cached, playback is instant — just like local files
- Swipe up on a Klipy artwork to pin it; swipe down to unpin

---

## Museum Channels (IIIF)

p3a can play artwork from major museums that publish their collections through the [IIIF Image API](https://iiif.io/api/image/3.0/). Channels are organized by the museum's own facets (collections, departments, sets, ...) and refreshed on a schedule. Seven museums ship today:

| Museum | Facets you can pick |
|---|---|
| **Art Institute of Chicago** | Departments, Classifications, Subjects, Themes, Galleries, Artwork types |
| **Rijksmuseum** | Curated Sets (Rijks's own collection groupings) |
| **Victoria and Albert Museum** | Collections, Categories, Venues |
| **Wellcome Collection** | Work types, Genres, Subjects, Contributors |
| **Statens Museum for Kunst (SMK)** | Collections |
| **Harvard Art Museums** | Classifications, Centuries, Cultures, Periods, Places, Media, Techniques, Work types, Groups, Galleries |
| **Smithsonian** | Museums (Cooper Hewitt, SAAM, NPG, NMAAHC, Hirshhorn, African Art) |

Five of the seven museums need no account or API key. Two do:

- **Harvard Art Museums** — request a free key at [harvardartmuseums.org/collections/api](https://harvardartmuseums.org/collections/api) (delivered by email within a day) and paste it into **Settings → Museums → Harvard Art Museums (API key)**.
- **Smithsonian** — register at [api.data.gov/signup/](https://api.data.gov/signup/) (instant, email-based) and paste the key into **Settings → Museums → Smithsonian (API key)**. One api.data.gov key covers Smithsonian and every other api.data.gov service (NASA, NOAA, etc.). Don't use `DEMO_KEY` — its ~30 req/hour-per-IP cap will throttle the first refresh mid-flight.

Until a key is saved, the corresponding museum entry in the browse modal will prompt you to add one and any channel you've already created will sit dormant (it reactivates the moment a valid key is saved — no need to re-create the channel).

A small number of Wellcome terms with very long labels are hidden from the browse modal — see [`docs/deferred/wellcome-long-labels.md`](deferred/wellcome-long-labels.md) for the rationale.

### Adding a museum channel

1. **Open the Playset Editor** at `http://p3a.local/playset-editor`
2. Open or create a playset and click **Add Channel**
3. Set **Channel Type** to **Museum**
4. A browse modal opens. Pick the museum, then the facet/axis (skipped for Rijks, which is axis-less), then the term you want
5. The modal previews one artwork at a time at 400 px, with **Previous** / **Next** navigation and a title/artist/date caption. The **Add** button commits the channel — not the visible artwork. Rijks previews resolve lazily (one 3-hop Linked Art walk per artwork) so the first thumbnail takes a moment
6. Click **Add channel** to push the channel into the playset, then save the playset normally

The channel saves under a name like `AIC · Arts of Greece, Rome, and Byzantium` or `V&A · Photographs`. The wire encoding is `{museum_id}:{axis}` for the channel name and the facet term id for the identifier — see [finalized-design.md §4.1](art-institutions/finalized-design.md) for the exact format.

### Refresh and download behavior

- **Refresh interval and cache size** are configured in **Settings → Museum** (`http://p3a.local/settings#museum`). Defaults: refresh every 1 day, keep up to 1024 artworks per channel.
- **First refresh** fetches the listing from the museum's API and stores it in the channel cache. Image downloads happen lazily as the device rotates through the playset — so the first artworks appear within seconds of the channel becoming active, and the rest fill in over the next minutes.
- **Cached files** live under `/sdcard/p3a/museum/{museum_id}/...` on the SD card. They're hash-sharded by IIIF identifier and shared across all channels of the same museum, so two channels that both reference the same artwork only ever pay for it once.
- **Rijks artworks** require a "Linked Art walk" (three follow-up HTTP requests per artwork) to discover the actual IIIF identifier. The device does this lazily in the background — one walk per download-task iteration — so a fresh Rijks channel takes ~50 minutes to fully populate but the first few artworks appear within seconds.
- **Rate limits** are honored per-museum. AIC publishes a 60-req/minute cap; the others don't publish one but a 429 still engages a cooldown. The cooldown is shared between the device's refresh and the browser-side browse modal: if you trigger throttling from the browse modal, the device also waits.
- **Storage management** is automatic. The age-based eviction in `components/storage_eviction/` reclaims museum cache files alongside the Makapix vault and Giphy cache when SD card free space drops below the configured target. No manual cleanup needed.

### Display name

Channel display names use the format `{museum_short} · {term label}` — e.g. `AIC · Drawing and Watercolor`, `Rijks · foto's`, `V&A · Sculpture`. The label is truncated with an ellipsis at the tail if the full string exceeds 64 characters.

---

## Device Registration

Register your p3a at [makapix.club](https://makapix.club/) to enable cloud features:

- **Send artworks directly** — browse artworks on the website and send them straight to your p3a
- **Remote control** — change artwork, adjust brightness from anywhere
- **Status monitoring** — see your device's online status
- **Send reactions** — swipe up on a Makapix artwork to give it a thumbs-up; swipe down to revoke

### How to register

1. **Open the settings page** at `http://p3a.local/settings.html`, go to the Makapix tab, and click **Enter Provisioning Mode**
2. **A registration code appears** on the display (6 characters)
3. **Go to [makapix.club](https://makapix.club/)** on your phone or computer
4. **Enter the registration code** and link it to your account
5. **The device connects automatically** via secure TLS MQTT

The registration code expires after 15 minutes. If it expires, click **Enter Provisioning Mode** again from the settings page to get a new code.

### Security

- Communication uses TLS 1.2 with mutual certificate authentication (mTLS)
- Each device gets unique client certificates during registration
- Commands are authenticated and encrypted end-to-end

---

## Makapix Club Features

Once your device is registered at [makapix.club](https://makapix.club/), you can use these cloud features:

### Sending artworks to your p3a

1. Browse artworks on [makapix.club](https://makapix.club/)
2. Click on any artwork to view it
3. Click the **"Send to p3a"** button
4. The artwork is sent directly to your device and displayed immediately

Supported formats: WebP, GIF, and APNG (animated, with transparency), PNG (still images, with transparency), and JPEG (still images).

### Remote control

From the Makapix Club website, you can:
- View your device's current status and artwork
- Navigate between artworks (next/previous)
- Adjust brightness
- Control playback (pause/resume)

---

## PICO-8 Monitor

p3a can act as a dedicated PICO-8 game display:

### How it works

1. Open `http://p3a.local/` in your browser
2. Click the **"PICO-8 Monitor"** link
3. Load any `.p8` or `.p8.png` cart file
4. The game streams wirelessly to the device via WebSocket
5. The browser runs a WebAssembly PICO-8 emulator (fake-08)
6. Frames are streamed at up to 30 FPS (adaptive — throttles down under network congestion) and upscaled to 720×720

### Auto-timeout

The device automatically exits PICO-8 mode after 30 seconds of inactivity (no frames received) and returns to normal artwork playback.

<p align="center">
  <img src="../images/pico-8/pico-8-gameplay.webp" alt="PICO-8 gameplay video">
</p>

---

## Troubleshooting

### Device doesn't appear as `p3a.local`

- mDNS doesn't work on all networks (especially corporate/guest networks)
- Find the device's IP address from your router's DHCP client list, or by long-pressing on the screen to show info screen
- Access the device at `http://<IP-ADDRESS>/` instead

### Touch not responding

- Clean the screen — the capacitive touch can be affected by moisture or debris
- Restart the device by unplugging and reconnecting power

### Can't connect to Wi-Fi

- Ensure you're entering the correct password
- The device supports WPA2 and WPA3 networks
- If the captive portal doesn't appear, manually browse to `http://p3a.local/` or `http://192.168.4.1`

### Need more help?

Check the [INFRASTRUCTURE.md](INFRASTRUCTURE.md) for technical details, or open an issue on the GitHub repository.

