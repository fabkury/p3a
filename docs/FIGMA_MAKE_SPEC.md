# p3a Web UI Specification for Figma Make

## Product Overview

p3a is a Wi-Fi pixel art player built on the ESP32-P4 platform. It displays animated artwork (WebP/GIF/PNG/JPEG) on a 720x720 IPS touchscreen. Content sources include:

- **Makapix Club** - A pixel art social network (channels: "all recent", "promoted", user-specific, hashtag-based)
- **Giphy** - Trending GIFs
- **SD Card** - Locally stored animation files
- **Custom Playsets** - User-defined playlists mixing multiple channels with configurable weighting

The device runs an HTTP server on port 80 (no authentication), accessible via mDNS at `http://p3a.local` (or `http://p3a-{name}.local` if a device name is configured). The web UI is the primary remote control interface, used from phones and desktops on the same LAN.

**Target audience**: Hobbyists and pixel art enthusiasts who own the device. The UI should be mobile-first but work well on desktop too.

**Brand**: Clean, modern, minimal. The device name is "p3a" (always lowercase). The current color scheme uses a purple-blue gradient background (#667eea to #764ba2) with white cards.

---

## Backend API Reference

All API endpoints are served at the device's IP on port 80. There is no authentication. All JSON responses follow this envelope:

```json
{ "ok": true, "data": { ... } }        // success
{ "ok": false, "error": "...", "code": "..." }  // error
```

Content-Type for JSON requests: `application/json`

### 1. Device Initialization and Status

#### GET /api/init
Combined initialization payload. Call this once on page load to get everything needed to render the main UI.

**Response:**
```json
{
  "ok": true,
  "data": {
    "ui_config": {
      "lcd_width": 720,
      "lcd_height": 720,
      "pico8_enabled": false
    },
    "channel_stats": {
      "all": { "total": 150, "cached": 42 },
      "promoted": { "total": 30, "cached": 12 },
      "giphy_trending": { "total": 50, "cached": 25 },
      "registered": true
    },
    "active_playset": "channel_recent",
    "play_order": 0,
    "paused": false,
    "playset_info": {
      "channel_count": 1,
      "total_cached": 42,
      "total_entries": 150,
      "exposure_mode": "equal",
      "pick_mode": "recency"
    }
  }
}
```

**Key fields:**
- `ui_config.pico8_enabled`: Whether PICO-8 streaming feature is available (show/hide PICO-8 link)
- `channel_stats.registered`: Whether the device is linked to a Makapix Club account
- `active_playset`: Name of the currently active playset (see Playset system below)
- `play_order`: 0=server order, 1=chronological, 2=random/shuffle
- `paused`: Whether auto-swap is paused
- `playset_info`: Statistics about the currently active playset

**Use case**: Main page load. Provides all data needed to render channel selector, shuffle toggle, pause state, and playset info in a single request.

---

#### GET /status
Detailed device status (for diagnostics/info display).

**Response:**
```json
{
  "ok": true,
  "data": {
    "state": "ready",
    "uptime_ms": 123456789,
    "heap_free": 1234567,
    "rssi": -52,
    "fw": {
      "version": "0.8.5-dev",
      "idf": "v5.5.1"
    },
    "queue_depth": 0,
    "device_name": "bedroom",
    "hostname": "p3a-bedroom",
    "api_version": 1,
    "wifi_recovery_reboots": 0,
    "touch_recovery_reboots": 0
  }
}
```

**Key fields:**
- `state`: App state - "ready", "processing", "error"
- `rssi`: Wi-Fi signal strength in dBm (null if unavailable). Typical range: -30 (excellent) to -90 (poor)
- `fw.version`: Current firmware version string
- `device_name`: User-set device name (empty string if not set)
- `hostname`: Effective mDNS hostname (e.g., "p3a" or "p3a-bedroom")

**Use case**: Info/about screen, device health monitoring, showing firmware version.

---

#### GET /api/state
Lightweight state snapshot for quick polling.

**Response:**
```json
{
  "ok": true,
  "data": {
    "state": "ready",
    "p3a_state": "ANIMATION_PLAYBACK",
    "uptime_ms": 123456789,
    "heap_free": 1234567,
    "rssi": -52,
    "current_post_id": 4567
  }
}
```

**Key fields:**
- `p3a_state`: Internal state machine state - "ANIMATION_PLAYBACK", "PROVISIONING", "OTA", etc.
- `current_post_id`: Makapix post ID currently displayed (null for SD card or unknown)

**Use case**: Polling for state changes, checking provisioning mode status.

---

#### GET /api/network-status
Network connection information.

**Response:**
```json
{
  "ok": true,
  "data": {
    "connected": true,
    "ssid": "MyWiFi",
    "ip": "192.168.1.42",
    "gateway": "192.168.1.1",
    "netmask": "255.255.255.0",
    "rssi": -52
  }
}
```

**Use case**: Network info/diagnostics screen, showing connection details.

---

#### GET /api/storage
SD card storage information.

**Response:**
```json
{
  "ok": true,
  "data": {
    "total_bytes": 31914983424,
    "free_bytes": 28589015040
  }
}
```

**Use case**: Storage management screen, showing used/free space with a progress bar.

---

### 2. Playback Controls

These endpoints control what the device is displaying right now.

#### POST /action/swap_next
Skip to the next artwork in the current channel/playset.

**Request body**: Empty or `{}`
**Response** (202):
```json
{ "ok": true, "data": { "queued": true, "action": "swap_next" } }
```

**Use case**: "Next" button. The action is queued asynchronously; the device will transition as soon as possible.

---

#### POST /action/swap_back
Go back to the previously displayed artwork.

**Request body**: Empty or `{}`
**Response** (202):
```json
{ "ok": true, "data": { "queued": true, "action": "swap_back" } }
```

**Use case**: "Previous/Back" button.

---

#### POST /action/pause
Pause auto-swap (the current artwork stays on screen indefinitely).

**Request body**: Empty or `{}`
**Response** (202):
```json
{ "ok": true, "data": { "queued": true, "action": "pause" } }
```

**Use case**: Pause/play toggle button (when currently playing).

---

#### POST /action/resume
Resume auto-swap after pausing.

**Request body**: Empty or `{}`
**Response** (202):
```json
{ "ok": true, "data": { "queued": true, "action": "resume" } }
```

**Use case**: Pause/play toggle button (when currently paused).

---

#### POST /action/show_url
Download an artwork from a URL and display it immediately.

**Request body:**
```json
{
  "artwork_url": "https://example.com/art.webp",
  "blocking": true
}
```

- `artwork_url` (required): URL to download
- `blocking` (optional, default true): Whether to block auto-swap while showing

**Response** (202):
```json
{ "ok": true, "data": { "queued": true, "action": "show_url" } }
```

**Use case**: "Play from URL" feature - paste a URL to an image/animation and display it.

---

#### POST /action/swap_to
Jump to a specific artwork within a channel.

**For SD card files:**
```json
{
  "channel": "sdcard",
  "filename": "my_art.gif"
}
```

**For Makapix artworks:**
```json
{
  "channel": "all",
  "post_id": 4567,
  "storage_key": "abc123...",
  "art_url": "/api/vault/ab/abc123.webp"
}
```

**Response:**
```json
{ "ok": true, "data": { "action": "swap_to" } }
```

**Use case**: Selecting a specific artwork from a file list or artwork browser.

---

### 3. Channel and Playset System

The device plays artwork from "channels" (content sources). A "playset" combines one or more channels with weighting and ordering rules.

#### Built-in channel names:
| Name | Description |
|------|-------------|
| `all` | All recent Makapix artworks |
| `promoted` | Curated/promoted Makapix artworks |
| `sdcard` | Local files from SD card |
| `giphy_trending` | Trending GIFs from Giphy |
| `followed_artists` | Artworks from followed Makapix users (server-managed) |

#### Channel types (for custom playsets):
| Type | Name field | Identifier field |
|------|-----------|-----------------|
| `named` | "all", "promoted" | (empty) |
| `sdcard` | "sdcard" | (empty) |
| `giphy` | "trending" or "search" | search query (for "search" type) |
| `user` | "user" | User's sqid |
| `hashtag` | "hashtag" | Hashtag string |

---

#### POST /channel
Switch to a channel (creates and activates a built-in playset).

**Request body** (one of):
```json
{ "channel_name": "all" }
{ "channel_name": "promoted" }
{ "channel_name": "sdcard" }
{ "hashtag": "pixelart" }
{ "user_sqid": "abc123" }
```

**Response:**
```json
{ "ok": true }
```

**Use case**: Channel selector buttons/tabs on the main screen.

---

#### GET /channel *(deprecated - use GET /playsets/active)*
Get current channel info.

**Response:**
```json
{
  "ok": true,
  "data": {
    "playset": "channel_recent",
    "channel_name": "all"
  }
}
```

---

#### GET /channels/stats
Get per-channel artwork counts.

**Response:**
```json
{
  "ok": true,
  "data": {
    "all": { "total": 150, "cached": 42 },
    "promoted": { "total": 30, "cached": 12 },
    "giphy_trending": { "total": 50, "cached": 25 },
    "registered": true
  }
}
```

- `total`: Total artworks known to exist in the channel (from server)
- `cached`: Artworks actually downloaded and available locally
- `registered`: Whether device has a Makapix player key (linked to account)

**Use case**: Showing artwork counts next to each channel option (e.g., "All (42/150)").

---

#### POST /playset/{name}
Load and activate a named playset. Tries local cache first, then fetches from Makapix server if connected.

**Response:**
```json
{
  "ok": true,
  "playset": "my_mix",
  "channel_count": 3,
  "from_cache": false,
  "builtin": false,
  "exposure_mode": "equal",
  "pick_mode": "recency",
  "total_cached": 120,
  "total_entries": 350
}
```

**Built-in playset names** (always available, no I/O needed):
- `channel_recent` - All recent Makapix artworks
- `channel_promoted` - Promoted artworks
- `channel_sdcard` - SD card files
- `giphy_trending` - Trending Giphy GIFs

**Use case**: Switching to a saved or built-in playset from a playset list.

---

#### GET /playsets
List all saved playsets (user-created, stored on SD card).

**Response:**
```json
{
  "ok": true,
  "data": {
    "playsets": [
      {
        "name": "my_mix",
        "channel_count": 3,
        "exposure_mode": "equal",
        "pick_mode": "recency"
      },
      {
        "name": "chill_vibes",
        "channel_count": 2,
        "exposure_mode": "manual",
        "pick_mode": "random"
      }
    ]
  }
}
```

**Use case**: Playset list/manager screen.

---

#### GET /playsets/active
Get the currently active playset with full details.

**Response:**
```json
{
  "ok": true,
  "data": {
    "name": "my_mix",
    "registered": true,
    "playset_info": {
      "channel_count": 3,
      "total_cached": 120,
      "total_entries": 350,
      "exposure_mode": "equal",
      "pick_mode": "recency",
      "channels": [
        {
          "display_name": "All Recent",
          "type": "named",
          "name": "all",
          "identifier": "",
          "available": 42,
          "total": 150,
          "refreshing": false,
          "last_refresh": 1709654321.0
        },
        {
          "display_name": "Trending GIFs",
          "type": "giphy",
          "name": "trending",
          "identifier": "",
          "available": 25,
          "total": 50,
          "refreshing": true,
          "last_refresh": 0.0
        }
      ]
    }
  }
}
```

**Use case**: Displaying active playset details, per-channel stats, refresh status.

---

#### GET /playsets/{name}[?activate=true]
Read a saved playset. Optionally activate it by adding `?activate=true`.

**Response:**
```json
{
  "ok": true,
  "data": {
    "exposure_mode": "equal",
    "pick_mode": "recency",
    "channels": [
      {
        "type": "named",
        "name": "all",
        "identifier": "",
        "display_name": "All Recent",
        "weight": 0
      }
    ]
  },
  "activated": false
}
```

**Use case**: Playset editor - loading a playset for viewing or editing.

---

#### POST /playsets/{name}
Create or update a playset.

**Request body:**
```json
{
  "exposure_mode": "equal",
  "pick_mode": "recency",
  "channels": [
    {
      "type": "named",
      "name": "all",
      "display_name": "All Recent",
      "weight": 0
    },
    {
      "type": "giphy",
      "name": "trending",
      "display_name": "Trending GIFs",
      "weight": 0
    }
  ],
  "activate": true,
  "rename_from": "old_name"
}
```

**Fields:**
- `exposure_mode` (string): "equal" (default), "manual" (use weight fields), "proportional"
- `pick_mode` (string): "recency" (newest first, default), "random"
- `channels` (array, 1-64 entries): Channel specifications
  - `type` (string, required): "named", "sdcard", "giphy", "user", "hashtag"
  - `name` (string, required): Sub-type (see channel types table above)
  - `identifier` (string): Required for "user" (sqid), "hashtag" (tag), and "giphy" search (query)
  - `display_name` (string, optional): Human-readable label
  - `weight` (number, optional): Weight for manual exposure mode (0 = auto)
- `activate` (boolean, optional): Immediately activate after saving
- `rename_from` (string, optional): If set, deletes the old playset (rename operation)

**Response:**
```json
{
  "ok": true,
  "data": {
    "saved": true,
    "activated": true,
    "renamed": false
  }
}
```

**Protected playsets** (cannot be overwritten or deleted): `followed_artists`

**Use case**: Playset editor - saving new or modified playsets.

---

#### DELETE /playsets/{name}
Delete a saved playset.

**Response:**
```json
{ "ok": true }
```

**Error responses:**
- 403: Protected playset (cannot delete)
- 404: Playset not found

**Use case**: Playset manager - deleting unwanted playsets.

---

### 4. Settings and Configuration

#### GET /config
Get the full configuration object (all settings in one blob).

**Response:**
```json
{
  "ok": true,
  "data": {
    "background_color": { "r": 0, "g": 0, "b": 0 },
    "show_fps": true,
    "max_speed_playback": false,
    "ppa_upscale": true,
    "dwell_time_ms": 30000,
    "channel_cache_size": 2048,
    "channel_select_mode": 1,
    "sdcard_root": "/p3a",
    "proc_notif_size": 64,
    "giphy_api_key": "",
    "giphy_rendition": "fixed_height",
    "giphy_format": "gif",
    "giphy_rating": "pg-13",
    "giphy_cache_size": 192,
    "giphy_refresh_interval": 3600,
    "giphy_prefer_downsized": true
  }
}
```

**Use case**: Loading settings page(s). All settings in one request.

---

#### PUT /config[?merge=true]
Save configuration. Without `merge=true`, replaces the entire config. With `merge=true`, only updates the fields provided (recommended).

**Request body** (example - partial merge):
```json
{
  "background_color": { "r": 32, "g": 32, "b": 32 },
  "dwell_time_ms": 15000
}
```

**Response:**
```json
{ "ok": true }
```

**Config fields reference:**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `background_color` | `{r,g,b}` | `{0,0,0}` | Background color for transparent artwork (0-255 each) |
| `show_fps` | boolean | true | Show FPS counter overlay on display |
| `max_speed_playback` | boolean | false | Skip frame timing (play as fast as possible) |
| `ppa_upscale` | boolean | true | Use hardware upscaling for Giphy (bilinear interpolation) |
| `dwell_time_ms` | number | 30000 | Auto-swap interval in milliseconds (0 = disabled, min 5000) |
| `channel_cache_size` | number | 2048 | Max artworks per Makapix channel (32-4096, reboot required) |
| `channel_select_mode` | number | 1 | Multi-channel selection: 0=SWRR (deterministic), 1=Stochastic (weighted random) |
| `sdcard_root` | string | "/p3a" | Root folder on SD card (reboot required) |
| `proc_notif_size` | number | 64 | Processing indicator triangle size in pixels (0=disabled, 16-256) |
| `giphy_api_key` | string | "" | Giphy API key (required for Giphy channels) |
| `giphy_rendition` | string | "fixed_height" | Giphy rendition: "fixed_height" or "original" |
| `giphy_format` | string | "gif" | Giphy format: "gif" or "webp" |
| `giphy_rating` | string | "pg-13" | Content rating filter: "g", "pg", "pg-13", "r" |
| `giphy_cache_size` | number | 192 | Max cached Giphy items per channel (32-500) |
| `giphy_refresh_interval` | number | 3600 | Giphy refresh interval in seconds |
| `giphy_prefer_downsized` | boolean | true | Prefer higher-quality downsized rendition when available (fixed_height only) |

**Use case**: Settings pages - save individual or grouped settings. Always use `?merge=true` to avoid overwriting unrelated settings.

---

#### GET /settings/dwell_time
**Response:**
```json
{ "ok": true, "data": { "dwell_time": 30 } }
```
Note: Returns seconds (not milliseconds).

#### PUT /settings/dwell_time
**Request body:**
```json
{ "dwell_time": 30 }
```
Value in seconds (0 = disable auto-swap, max 100000).

---

#### GET /settings/play_order
**Response:**
```json
{ "ok": true, "data": { "play_order": 0 } }
```

#### PUT /settings/play_order
**Request body:**
```json
{ "play_order": 2 }
```
Values: 0 = server/default order, 1 = chronological, 2 = random/shuffle

**Use case**: Shuffle toggle. Value 2 enables shuffle mode, other values disable it.

---

#### GET /settings/refresh_override
**Response:**
```json
{ "ok": true, "data": { "refresh_allow_override": false } }
```

#### PUT /settings/refresh_override
**Request body:**
```json
{ "refresh_allow_override": true }
```

**Use case**: One-time bypass to force all channels to refresh their content from server. Checkbox in advanced settings.

---

#### GET /rotation
**Response:**
```json
{ "ok": true, "rotation": 0 }
```
Values: 0, 90, 180, 270 (degrees)

#### POST /rotation
**Request body:**
```json
{ "rotation": 90 }
```
Valid values: 0, 90, 180, 270

**Use case**: Screen rotation control. The display physically rotates the image.

---

### 5. Device Identity

#### GET /api/device-name
**Response:**
```json
{
  "ok": true,
  "device_name": "bedroom",
  "hostname": "p3a-bedroom"
}
```

#### POST /api/device-name
**Request body:**
```json
{ "device_name": "bedroom" }
```
Rules: lowercase `[a-z0-9-]`, max 16 chars, no leading/trailing hyphen. Empty string clears the name.

**Response:**
```json
{
  "ok": true,
  "device_name": "bedroom",
  "hostname": "p3a-bedroom",
  "reboot_required": true
}
```

**Use case**: Device naming for multi-device households. After setting a name, the device becomes accessible at `http://p3a-bedroom.local`. Requires reboot to take effect.

---

### 6. File Upload

#### POST /upload
Upload an artwork file to the SD card. Multipart/form-data.

**Request**: `Content-Type: multipart/form-data` with a file field
- Max file size: 5 MB
- Accepted extensions: `.webp`, `.gif`, `.jpg`, `.jpeg`, `.png`

**Response:**
```json
{
  "ok": true,
  "data": {
    "filename": "my_art.gif",
    "message": "File uploaded and playing"
  }
}
```

**Error responses:**
- 413: File too large (>5MB)
- 415: Wrong content type
- 423: SD card is shared over USB (locked)

**Side effect**: After upload, the device automatically switches to the SD card channel and starts playing.

**Use case**: Upload artwork from phone/desktop to display on the device.

---

### 7. Firmware Updates (OTA)

#### GET /ota/status
**Response:**
```json
{
  "ok": true,
  "data": {
    "state": "idle",
    "current_version": "0.8.5-dev",
    "available_version": "0.9.0",
    "available_size": 4194304,
    "release_notes": "Bug fixes and improvements",
    "last_check": 1709654321.0,
    "download_progress": null,
    "error_message": null,
    "can_rollback": true,
    "rollback_version": "0.8.4",
    "dev_mode": false,
    "is_prerelease": false
  }
}
```

**OTA states**: `"idle"`, `"checking"`, `"update_available"`, `"downloading"`, `"installing"`, `"error"`

---

#### POST /ota/check
Trigger an update check. Returns 202 immediately; poll `/ota/status` for results.

**Response** (202):
```json
{ "ok": true, "data": { "checking": true, "message": "Update check started" } }
```

---

#### POST /ota/install
Start firmware installation. The device will reboot when complete.

**Preconditions**: State must be `"update_available"`. Returns 409 if no update available, 423 if blocked.

**Response** (202):
```json
{
  "ok": true,
  "data": {
    "installing": true,
    "message": "Firmware update started. Device will reboot when complete."
  }
}
```

---

#### POST /ota/rollback
Roll back to previous firmware version and reboot.

**Response** (202):
```json
{
  "ok": true,
  "data": {
    "rolling_back": true,
    "target_version": "0.8.4",
    "message": "Rolling back. Device will reboot."
  }
}
```

---

#### GET /ota/webui/status
Web UI update status (the web UI itself can be updated separately from firmware).

**Response:**
```json
{
  "ok": true,
  "data": {
    "current_version": "1.2.0",
    "available_version": "1.3.0",
    "update_available": true,
    "partition_valid": true,
    "needs_recovery": false,
    "auto_update_disabled": false,
    "failure_count": 0,
    "state": "idle",
    "progress": 0,
    "status_message": null,
    "error_message": null
  }
}
```

---

#### POST /ota/webui/repair
Force re-download of the web UI (repairs corrupted web UI partition).

**Response** (202):
```json
{ "ok": true, "data": { "repairing": true, "message": "Web UI repair started" } }
```

---

### 8. Wi-Fi Setup and Provisioning

#### Wi-Fi Initial Setup (Captive Portal)

When the device has no saved Wi-Fi credentials, it creates its own access point named "p3a-setup" (open, no password). Users connect to this AP and are redirected to a captive portal setup wizard.

**Setup pages** (served only in AP/setup mode):
- `GET /setup` - Wi-Fi configuration form (SSID, password, optional device name)
- `POST /save` - Submit Wi-Fi credentials (form-encoded: `ssid`, `password`, `device_name`). Device reboots and joins the configured network.
- `GET /setup/success` - Setup success confirmation
- `GET /setup/error` - Setup error screen
- `GET /setup/erased` - Credentials erased confirmation

These pages are only relevant during initial setup or after a Wi-Fi reset. The new web UI does **not** need to redesign these pages - they are a separate flow. Mentioning them here only for completeness.

---

#### POST /erase
Erase saved Wi-Fi credentials and reboot. The device will enter Wi-Fi setup mode (captive portal) on next boot.

**Request body**: None
**Response:**
```json
{ "ok": true, "message": "WiFi credentials erased. Rebooting..." }
```

**Warning**: This disconnects the device from Wi-Fi. After reboot, the device creates its own AP for Wi-Fi setup.

**Use case**: "Forget Wi-Fi / Reset Network" button with a confirmation step.

---

#### POST /action/provision
Enter or exit Makapix Club provisioning mode.

**Request body:**
```json
{ "enable": true }
```

**Response:**
```json
{
  "ok": true,
  "data": { "action": "provision", "enabled": true }
}
```

**Use case**: Link/unlink the device with a Makapix Club account. When provisioning is active, the device displays a pairing code on screen.

---

#### POST /action/reboot
Reboot the device.

**Request body**: Empty or `{}`
**Response** (202):
```json
{ "ok": true, "data": { "queued": true, "action": "reboot" } }
```

**Use case**: Reboot button (with confirmation).

---

### 9. PICO-8 Integration (Optional Feature)

Only available when `pico8_enabled` is true in `/api/init` response.

#### GET /pico8
Serves the PICO-8 streaming monitor page. When this page is opened, the device enters PICO-8 mode.

#### WebSocket /pico_stream
Binary WebSocket for streaming PICO-8 framebuffer data. Custom binary protocol.

#### POST /pico8/exit
Exit PICO-8 mode (called by browser on page unload).

**Use case**: PICO-8 fantasy console streaming. The device runs PICO-8 games and streams the display to the browser. This is a self-contained page with its own WebSocket-based rendering.

---

## Key Use Cases and Capabilities

The web UI should support these user workflows:

### Playback Control
- Skip forward/backward through artworks (next/previous buttons)
- Pause/resume auto-swap (play/pause toggle)
- See what's currently playing (active playset name, channel info)

### Channel Selection
- Switch between content sources (Makapix channels, Giphy, SD card)
- See how many artworks are available in each channel (cached vs total)
- Browse channels by hashtag or user (Makapix)

### Playset Management
- View list of saved playsets
- Create custom playsets mixing multiple channels
- Edit existing playsets (change channels, weights, modes)
- Rename and delete playsets
- Activate a playset

### File Upload
- Upload artwork files (WebP/GIF/PNG/JPEG, max 5MB) from phone or desktop
- Automatic playback after upload

### Device Settings
- **Display**: Background color (RGB), processing indicator size, FPS overlay, max speed playback, PPA upscale, auto-swap interval, screen rotation
- **Playback**: Play order (server/chronological/shuffle), channel selection mode (SWRR/Stochastic), refresh override
- **Storage**: View SD card usage (total/free/used with progress bar), set channel cache size, set SD card root path
- **Giphy**: API key, rendition (fixed_height/original), format (GIF/WebP), content rating, cache size, refresh interval, prefer-downsized toggle

### Device Management
- Set device name (for mDNS hostname)
- View device info (firmware version, uptime, Wi-Fi signal, heap)
- Network info (IP, SSID, gateway)
- Reboot device
- Reset Wi-Fi (forget credentials)
- Link/unlink Makapix Club account (provisioning)

### Firmware Updates
- Check for updates
- View available version and release notes
- Install firmware update (device reboots)
- Rollback to previous version
- Web UI repair (re-download corrupted web UI)

### PICO-8 (conditional)
- Navigate to PICO-8 streaming page (only shown if feature is enabled)
- Self-contained page with WebSocket-based framebuffer streaming

---

## Technical Constraints

- **No framework dependencies**: The device has 4MB of flash for the web UI (LittleFS). Keep bundle sizes minimal. Vanilla HTML/CSS/JS is acceptable. If using a framework, the total built output must be small.
- **Gzip support**: The device serves `.gz` versions of files automatically. Build assets can be gzipped.
- **No WebSocket** (except PICO-8): All main UI communication is HTTP request/response. No real-time push. Use polling only if needed (e.g., OTA progress).
- **Mobile-first**: Primary use is from phones on the same Wi-Fi. Touch-friendly, responsive.
- **No authentication**: The device is on a local network. No login/auth needed.
- **Offline-capable content**: The UI must work without internet access (no CDN dependencies). All assets must be self-contained.
- **ESP32 server limitations**: Max 12 concurrent sockets shared across all connections. Minimize concurrent requests. The `/api/init` endpoint exists specifically to combine multiple data fetches into one request.
