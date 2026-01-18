# p3a Roadmap

> **Project**: Internet-connected pixel art player  
> **Board**: Waveshare ESP32-P4-WiFi6-Touch-LCD-4B (720×720 square IPS touch)  
> **SDK**: ESP-IDF v5.3+ / v5.5 (esp32p4 target)

---

## Goals

* Render pixel art from URLs (GitHub Pages) on 720×720 display with canvas constraints (e.g., 128×128)
* Subscribe to MQTT over TLS, receive notifications, fetch & cache artwork, play
* UI: network setup, MQTT config, brightness, playlist control, diagnostics
* Robust OTA, logging, safe defaults

**Non-goals**: Authoring tools, cameras, heavy animations

**Security**: TLS provisioning, certificate-pinned MQTT, backend-enforced content authenticity

---

## 1) Environment & SDK

**1.1 Toolchain & skeleton**
- ESP-IDF v5.5 (esp32p4 target), `-Os` + LTO
- Components: `board/`, `net/`, `ui/`, `renderer/`, `mqtt/`, `storage/`, `ota/`, `telemetry/`, `hal/`
- **AC**: Boots "hello world", logs chip & IDF versions

---

## 2) Board Support Package

**2.1 Peripherals**
- Map pins: LCD (MIPI-DSI), backlight, touch (I²C), MicroSD, ESP32-C6 control
- `board/pins.h` + `board_init()` for power-up
- **AC**: Pin map documented, backlight toggle works

**Note**: ESP32-C6-MINI-1 provides Wi-Fi 6/BLE; confirm interconnect (UART/SPI/SDIO)

---

## 3) Display & Touch

**3.1 LCD** ✅ IMPLEMENTED
- ST7703 MIPI-DSI controller via Waveshare BSP
- Frame buffer 720×720 RGB565/RGB888 (configurable)
- Multi-buffer rendering with PSRAM
- **AC**: Solid color + checkerboard visible, no flicker

**3.2 Touch** ✅ IMPLEMENTED
- GT911 I²C touch controller
- `touch_read()` with rotation mapping
- Gestures: tap, vertical swipe, long-press
- **AC**: Touch demo prints coordinates

**3.3 UI Framework** ✅ IMPLEMENTED
- µGFX for registration code display
- Web UI for status/controls via HTTP
- **AC**: UI renders correctly

---

## 4) Storage & Caching

**4.1 Filesystem** ✅ IMPLEMENTED
- SPI flash: NVS + SPIFFS for config and web UI
- MicroSD: artwork storage via SDMMC
- Partitions: `nvs`, `phy_init`, `factory`, `storage` (SPIFFS)
- **AC**: Read/write settings, SD access works

**4.2 Cache Policy** ⏳ PENDING
- LRU cache on SD: asset ID/URL → `{meta, image.bin}`
- Cap: 256 MB, purge oldest on low space
- **AC**: Cache hits avoid network, purge works
- **Note**: Awaiting feed ingestion implementation

---

## 5) Networking

**5.1 Wi-Fi** ✅ IMPLEMENTED
- ESP32-C6 Wi-Fi 6 bring-up
- Provisioning: SoftAP+Captive Portal
- **AC**: Connects WPA2/WPA3, persists credentials, handles failures

**5.2 TLS** ✅ IMPLEMENTED
- ESP-IDF certificate bundle for HTTPS provisioning
- Client certificates (mTLS) for MQTT authentication
- **AC**: TLS handshake succeeds, invalid certs rejected

---

## 6) MQTT Client

**6.1 Client** ✅ IMPLEMENTED
- ESP-IDF `esp-mqtt` with TLS
- Config from NVS: broker, port, client_id, cert/key/token, topics
- **Status**: Device registration at makapix.club, mTLS authentication, status publishing every 30s

**6.2 Topics & Payloads** ✅ IMPLEMENTED
- Subscribe: `makapix/player/{player_key}/command` ✅
- Publish: `makapix/player/{player_key}/status` ✅
- **Implemented**: Status messages, commands (swap_next, swap_back, etc.)
- **Artwork sending** from makapix.club directly to device ✅
- Payload schema (JSON): `{post_id, artist, playlist?, assets:[{url, sha256, w,h,format,frame_delay?}], expires_at}`
- **AC**: Message triggers display, download support ✅

**Security**: Client certs/tokens ✅, subscribe-only ACLs

---

## 7) Downloader & Content Integrity

**7.1 HTTP(S) Fetcher** ⏳ PENDING
- Streaming GET with timeouts, size limit (1.5 MB/asset), gzip support
- **Note**: HTTPS client exists for provisioning; artwork download from feeds not yet implemented

**7.2 Validation** ⏳ PENDING
- Size/time limits, MIME sniffing (magic bytes)
- Reject SVG, unsupported formats
- **AC**: Invalid assets rejected, logged

---

## 8) Renderer & Playback

**8.1 Decoders** ✅ IMPLEMENTED
- WebP (libwebp), PNG (libpng), JPEG (esp_jpeg), GIF (animated_gif)
- **Transparency/alpha channel support** for WebP, GIF, and PNG
- Normalize to display format, center/pad, nearest-neighbor upscale
- **Aspect ratio preservation** for non-square artworks
- **Configurable background color** for transparent images and letterboxing

**8.2 Frame Pipeline** ✅ IMPLEMENTED
- Multi-buffer rendering with PSRAM
- Prefetching adjacent animations
- Max-speed or frame-timed playback modes

**8.3 Playlists** ⏳ PARTIAL
- Sequential playback with auto-advance ✅
- Touch "Next/Prev" ✅
- **AC**: 128×128 art crisp/centered, playlist loops, no tearing ✅
- JSON-based playlist management: PENDING

---

## 9) UI/UX

**9.1 First-run Wizard**
- Language, Wi-Fi, MQTT (QR scan), brightness, time sync

**9.2 Home Screen**
- Artwork viewport, status icons, quick actions (pause, brightness, cache clear)

**9.3 Settings**
- Wi-Fi, MQTT, About (chip rev, IDF ver), storage, factory reset

**AC**: Touch navigation, long-press corner opens Settings

---

## 10) OTA & Reliability

**10.1 OTA** ✅ IMPLEMENTED
- ESP-IDF OTA dual slots (ota_0, ota_1 partitions)
- GitHub Releases API integration for update discovery
- Automatic periodic checks (every 2 hours)
- Web UI for manual check, install, and rollback
- SHA256 checksum verification
- Progress display on LCD during updates
- **ESP32-C6 co-processor auto-flash** — firmware is updated automatically when needed
- **AC**: Power-loss-safe ✅, automatic rollback on 3 boot failures ✅, version in web UI ✅

**10.2 Watchdogs & Recovery** ⏳ PARTIAL
- Task WDT ✅
- Network heartbeat: via MQTT status publishing
- Boot loops → automatic OTA rollback ✅
- Safe mode recovery screen: PENDING

**10.3 Logs/Diagnostics**
- Ring buffer, dump to SD, "Send diag" over MQTT (compressed)

---

## 11) Security Hardening

- TLS everywhere, MQTT cert pinning
- NVS credentials encrypted
- JSON schema validation, reject unknown fields
- URL host allowlist (`*.github.io`, `raw.githubusercontent.com`)
- Cache by asset ID, expire by `expires_at`
- No SVG, strict PNG/JPEG/GIF sniffing
- Rate-limit `device/{id}/cmd`

---

## 12) Performance & Power

- Backlight PWM curve, max current limits
- Idle dim after N min, sleep if idle M min, wake on touch/timer
- Predecode next image when idle, document RAM budget (PSRAM if present)

---

## 13) Manufacturing & Provisioning

- **Provisioning Tool** (Python/IDF script): flash app+bootloader+partitions, device ID, cert/key/token, broker URL
- Print QR label (broker URI + device id)
- **AC**: Fresh device boots to Home within 30s after network join

---

## 14) Testing

**Unit**: Decoders (golden vectors), cache LRU, JSON parsing

**Hardware**: Wi-Fi reconnect soak, MQTT storm, SD hot-plug, power-cycle during OTA

**Display**: Color bars, gamma chart, touch accuracy (5-point), FPS measurement

**Security**: Invalid certs, host violations, huge payloads, MIME spoofing, SVG injection

**Benchmarks**: Render latency (128×128→frame), memory footprint, download-to-display time

---

## 15) CI/CD

- GitHub Actions: build `esp32p4`, unit tests, signed OTA artifacts
- Versioning: `YY.MM.patch` + git SHA

---

## 16) Documentation

- `/docs/board-notes.md`: pin map, Waveshare links
- `/docs/networking.md`: provisioning, MQTT topics, payload schemas
- `/docs/ota.md`: update flow, rollback rules
- End-user quickstart

---

## 17) Execution Plan

**Phase A — Bring-up** ✅ COMPLETE: §1, §2, §3.1–3.2 → LCD colors, touch readout, UART logs

**Phase B — Core I/O** ✅ COMPLETE: §4, §5, §6 → Wi-Fi, TLS MQTT connection, device registration

**Phase C — Playback** ⏳ IN PROGRESS: §7–§8, §9.2 → Local playback works; feed ingestion pending

**Phase D — Polish** ⏳ IN PROGRESS: §9.1, §10, §11, §12, §13 → OTA ✅, recovery partial, reactions pending

**Phase E — Test & Ship** (upcoming): §14, §15, §16 → CI releases v1.0

### Current Status (December 2025)

Completed:
- Display pipeline with multi-buffer rendering
- Animation playback from SD card with prefetching
- **Transparency/alpha channel support** for WebP, GIF, and PNG
- **Aspect ratio preservation** for non-square artworks
- **Configurable background color** for transparent images
- Touch gestures (tap, swipe, long-press for registration)
- Screen rotation (0°, 90°, 180°, 270°) with touch gesture and API
- Wi-Fi provisioning with captive portal
- Local web UI and REST API
- Device registration at makapix.club
- TLS MQTT client with mTLS authentication
- **Makapix Club integration** — send artworks directly from makapix.club
- Remote control from website (commands via MQTT)
- USB composite device (serial + mass storage)
- **OTA firmware updates** from GitHub Releases with automatic rollback
- **ESP32-C6 co-processor auto-flash** — Wi-Fi module firmware updated automatically

In Progress:
- Hardware reactions (send likes to artworks)

Blocked (external dependency):
- Browser-based web flasher — blocked by [esptool-js bug](https://github.com/espressif/esptool-js/issues/229)

---

## 18) Directory Layout

```
firmware/
  CMakeLists.txt
  sdkconfig.defaults
  main/
    app_main.c
    app_events.h
  components/
    board/      pins.h pins.c board.c
    hal/        lcd_*.c touch_*.c backlight.c
    net/        wifi.c tls.c http_fetch.c
    mqtt/       client.c proto/
    renderer/   decoder_png.c decoder_jpeg.c blit.c
    storage/    kv.c fs.c cache.c
    ui/         ui_root.c lvgl_port.c
    ota/        ota.c
    telemetry/  log.c diag.c
  docs/
```

---

## 19) Acceptance Tests

- **AT-01**: MQTT `posts/new` with 128×128 PNG → download & display <2s
- **AT-02**: Invalid payload size/MIME → reject, log, wait for next
- **AT-03**: Power loss during OTA → auto rollback on boot
- **AT-04**: No network → cached artworks rotate locally
- **AT-05**: Touch opens Settings, brightness persists
- **AT-06**: Reject SVG or content-type mismatch
- **AT-07**: MQTT cert expired → connection refused, user alerted

---

## 20) Open Questions

- LCD controller & touch IC part numbers (schematic/BOM)
- ESP32-C6 interface to P4 (UART vs SDIO/SPI) and Wi-Fi bring-up path
- PSRAM fitted & supported for larger frame buffers
