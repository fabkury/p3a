# Network and API

## Wi-Fi Management (wifi_manager component)

### Architecture

- **Mode**: Station (STA) with AP fallback
- **Driver**: ESP-WiFi-Remote (offloads to ESP32-C6 via ESP-Hosted)
- **Stack**: LwIP TCP/IP

### Provisioning Flow

1. **Boot**: Attempts to connect using saved NVS credentials
2. **Failure**: Starts captive portal AP mode (`p3a-setup`)
3. **Configuration**: User submits SSID/password via the captive portal form (served at `/`, posts to `/save`)
4. **Reconnection**: Device saves credentials to NVS and reboots into STA mode

### mDNS

- **Hostname**: `p3a.local` by default; `p3a-<device_name>.local` when a device name is configured (in AP/setup mode, both resolve via a delegate hostname)
- **Service**: `_http._tcp` advertised on port 80

### Kconfig

- `WIFI_MANAGER_ENABLED` — enable/disable WiFi manager

---

## HTTP Server (http_api component)

The HTTP server is implemented across multiple source files in `components/http_api/`:

| File | Responsibility |
|------|---------------|
| `http_api.c` | Server init, routing, catch-all handlers |
| `http_api_rest_status.c` | `/status` and `/api/*` endpoints |
| `http_api_rest_actions.c` | `/action/*` endpoints |
| `http_api_rest_settings.c` | `/settings/*` and `/config` endpoints |
| `http_api_rest_playsets.c` | `/playsets/*` endpoints |
| `http_api_rest_museum.c` | `/api/museum/*` rate-limit endpoints |
| `http_api_ota.c` | `/ota/*` endpoints |
| `http_api_upload.c` | `/upload` endpoint |
| `http_api_pages.c` | HTML page serving |
| `http_api_pico8.c` | PICO-8 WebSocket handler |

---

## REST API Endpoints

### Status and Init

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/status` | Device status (uptime, heap, RSSI, firmware info) |
| GET | `/api/init` | Combined init payload (ui_config, channel_stats, active_playset, paused, playset_info, current_artwork) |
| GET | `/api/ui-config` | UI configuration (LCD dimensions, feature flags) |
| GET | `/api/network-status` | Network connection info (IP, SSID, RSSI) |
| GET | `/api/state` | Lightweight state snapshot |

### Configuration

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/config` | Current configuration as JSON |
| PUT | `/config` | Update configuration (supports `?merge=true` for partial updates) |

### Settings

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/settings/dwell_time` | Get dwell time |
| PUT | `/settings/dwell_time` | Set dwell time |
| GET | `/settings/refresh_override` | Get refresh override |
| PUT | `/settings/refresh_override` | Set refresh override |
| GET | `/rotation` | Get screen rotation |
| POST | `/rotation` | Set screen rotation |

> Pick mode (recency vs random) is part of each playset definition (POST `/playsets/{name}`) — there is no dedicated `/settings/play_order` endpoint.

### Actions

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/action/reboot` | Reboot device |
| POST | `/action/swap_next` | Advance to next artwork |
| POST | `/action/swap_back` | Go to previous artwork |
| POST | `/action/pause` | Pause playback |
| POST | `/action/resume` | Resume playback |
| POST | `/action/show_url` | Download and play artwork from a URL |
| POST | `/action/swap_to` | Swap to a specific artwork (SD card or Makapix) |

### Channels

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/channels/stats` | Channel cache statistics |
| GET | `/channel` | Current channel/playset info (deprecated) |
| POST | `/channel` | Switch channel (all/promoted/sdcard/hashtag/user) |

### Playsets

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/playsets` | List all saved playsets |
| GET | `/playsets/active` | Get active playset name |
| GET | `/playsets/{name}` | Read a playset (optionally activate with `?activate=true`) |
| POST | `/playsets/{name}` | Create/update a playset (optionally activate) |
| DELETE | `/playsets/{name}` | Delete a saved playset |
| POST | `/playset/{name}` | Load and execute a named playset |

### Museum (IIIF) Channels

The full design for museum channels lives in [`docs/art-institutions/finalized-design.md`](../art-institutions/finalized-design.md). The HTTP surface for them is small — channels themselves flow through `/playsets/{name}` like any other channel; the two endpoints below exist only to keep the per-museum rate-limit cooldown synchronized between the device and the browser.

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/museum/rate-limits` | Per-museum cooldown table. Returns `{ "artic": { "remaining_sec": N }, "rijks": { ... }, ... }`. The browse modal polls this before triggering its term-count probes. |
| POST | `/api/museum/rate-limits/report-429` | Browser reports a 429 it received directly from a museum API so the device's cooldown stays accurate. Body: `{ "museum": "artic", "retry_after_sec": 38 }`. `retry_after_sec` is optional; omitted/zero engages the museum-specific default (60 s today). Unknown museum ids are silently accepted. |

### File Upload

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/upload` | Multipart file upload (max 16 MiB, saves to animations dir) |

### OTA Updates

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/ota/status` | OTA status (version info, update availability) |
| GET | `/ota/webui/status` | Web UI OTA status |
| POST | `/ota/check` | Trigger OTA update check |
| POST | `/ota/install` | Start firmware installation |
| POST | `/ota/rollback` | Rollback to previous firmware |
| POST | `/ota/webui/install` | Install pending web UI update |
| POST | `/ota/webui/repair` | Force re-download of web UI |

### Wi-Fi

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/erase` | Erase Wi-Fi credentials and reboot |

### PICO-8

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/pico8/exit` | Exit PICO-8 mode (if `CONFIG_P3A_PICO8_ENABLE`) |

---

## Web Pages

Static HTML pages served from the LittleFS partition:

| URL | Source File | Description |
|-----|------------|-------------|
| `/` | `webui/index.html` | Main control page |
| `/settings` | `webui/settings.html` | Settings page |
| `/config/network` | `webui/network.html` | Network configuration |
| `/giphy` | `webui/giphy.html` | Giphy settings |
| `/playset-editor` | `webui/playset-editor.html` | Playset editor |
| `/pico8` | `webui/pico8/index.html` | PICO-8 monitor (if enabled) |
| `/pico8/*` | `webui/pico8/*` | PICO-8 module assets (CSS/JS/WASM/PNG, if enabled) |
| `/ota` | `webui/ota.html` | OTA update page |
| `/static/*` | `webui/static/*` | Shared CSS, JS, and image assets |

---

## WebSocket

- **Endpoint**: `/pico_stream`
- **Protocol**: Binary PICO-8 frames (palette + indexed pixels)
- **Condition**: Only available when `CONFIG_P3A_PICO8_ENABLE` is set

---

## MQTT (makapix component)

- **Protocol**: MQTT over TLS 1.2 with mTLS authentication (TLS 1.3 is disabled in `sdkconfig`)
- **Broker**: Makapix Club cloud
- **Features**: Remote commands (`swap_next`, `swap_back`, `play_channel`, `show_artwork`, `show_url`, `swap_to`, `execute_playset`, `set_background_color`), artwork receiving, view tracking, status publishing via MQTT LWT
- See [Components — makapix](components.md#11-makapix--makapix-club-integration) for details.
