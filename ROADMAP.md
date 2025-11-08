# Firmware Roadmap — P3A

> Project name: P3A.
> Project description: Internet-connected pixel art player based on ESP32-P4-WiFi6-Touch-LCD-4B.
> Target board: Waveshare ESP32-P4-WiFi6-Touch-LCD-4B (ESP32-P4 HMI SoC + onboard ESP32-C6 for Wi-Fi 6/BLE; 4" round 720×720 IPS touch)
> SDK: **ESP-IDF v5.3+ / v5.5 (stable “esp32p4” target)**

This roadmap is written to be executed line-by-line by an AI agent. Each task has: **goal**, **steps**, **deliverables**, **acceptance criteria (AC)**, and **notes**. Keep this file updated as the single source of truth.

---

## 0) Project framing

**Goals**

* Render pixel artworks from URLs (GitHub Pages) on the 720×720 round display, respecting canvas constraints (e.g., 128×128 primary), with optional letterbox/center.
* Subscribe to MQTT topics over TLS, receive “new post” notifications, fetch artwork(s) over HTTPS, cache locally, and play.
* Provide simple UI: network setup, MQTT credentials enrollment, brightness, playlist control, diagnostics.
* Robust OTA, logs, and safe defaults.

**Non-goals**

* Authoring tools on device, cameras, or heavy animations beyond basic frame timing.

**Threat model highlights**

* Device provisioning over TLS; certificate-pinned MQTT; content hashing to detect swapped assets (see §7).

---

## 1) Environment & SDK bring-up

**1.1 Toolchain & project skeleton**
**Steps**

1. Set up **ESP-IDF v5.5 (stable) or v5.3+** with **esp32p4** target. The current environment has ESP-IDF version v5.5.1 installed at IDF_PATH = "C:\Users\Fab\esp\v5.5.1\esp-idf" and IDF_TOOLS_PATH = "C:\Users\Fab\.espressif".
2. `idf.py set-target esp32p4`; create project from `idf.py create-project p3a`.
3. Enable CMake presets; set `-Os`, LTO, and `CONFIG_COMPILER_OPTIMIZATION_SIZE=y`.
4. Add `components/` layout: `board/`, `net/`, `ui/`, `renderer/`, `mqtt/`, `storage/`, `ota/`, `telemetry/`, `hal/`.

**Deliverables**: Booting “hello world” over UART; Git repo initialized.
**AC**: Flash + run; boot log shows chip & IDF versions.

**Notes**: Track **chip revision** at boot and log it (some features vary by rev).

---

## 2) Board support package (BSP)

**2.1 Confirm peripherals from schematic**
**Steps**

1. From Waveshare schematic, list key pins: LCD (MIPI-DSI), backlight, touch (I²C), MicroSD, codec/audio (optional), ESP32-C6 control lines, and user buttons. 
2. Create `board/pins.h` mapping; document GPIOs & busses.
3. Implement `board_init()` to power-up rails/backlight safely.

**Deliverables**: `board/README.md` + `pins.h/c`.
**AC**: Build prints pin map; toggling backlight doesn’t brown-out.

**Notes**: The board integrates **ESP32-C6-MINI-1** module for Wi-Fi 6/BLE. We’ll use it as the RF front-end where appropriate; confirm interconnect (UART/SPI/SDIO) from the schematic.

---

## 3) Display & touch bring-up

**3.1 LCD panel identification & basic draw**
**Steps**

1. Identify the LCD controller (from schematic/BOM or Waveshare docs). If undocumented, probe via DSI init sequences provided by Waveshare; otherwise contact vendor or inspect driver code.
2. Use ESP-IDF **MIPI-DSI** driver (if available for P4) or vendor driver.
3. Initialize frame buffer @ 720×720 RGB565 or RGB888 depending on bandwidth; start with solid color fill, then test patterns.

**Deliverables**: `hal/lcd_*.[ch]` with `lcd_init()`, `lcd_draw_bitmap()`.
**AC**: Solid color + checkerboard test visible; no flicker/tearing.

**3.2 Touch controller bring-up**
**Steps**

1. Discover touch IC on I²C (common: FT5x06/GT911).
2. Implement `touch_read()` returning (x,y,pressure) with rotation mapping.

**Deliverables**: `hal/touch_*.[ch]`.
**AC**: Basic touch demo prints coordinates; simple “touch to draw dot” works.

**3.3 UI framework**
**Steps**

1. Integrate **LVGL** (esp-lvgl-port). Create a driver that posts flushed regions to LCD.
2. Build a minimal UI: status bar (Wi-Fi/MQTT), FPS, brightness slider.

**AC**: LVGL “widgets” demo runs ≥ 30 FPS with backlight control.

---

## 4) Storage & caching

**4.1 Filesystem**
**Steps**

1. Mount **SPI flash** partition for config (`NVS` + `spiffs` or `littlefs`), and **microSD** for artwork cache (if populated).
2. Define partitions: `nvs`, `phy_init`, `otadata`, `ota_0`, `ota_1`, `app`, `spiffs`.

**Deliverables**: `storage/kv.c` (NVS helpers), `storage/fs.c`.
**AC**: Read/write settings; SD card hot-plug detected; capacity printed.

**4.2 Cache policy**
**Steps**

1. LRU cache on SD: key = SHA256(url) → `{meta, image.bin}`.
2. Cap by size (e.g., 256 MB) and count; purge oldest on low space.

**AC**: Cached image hits avoid network; purge works under stress.

---

## 5) Networking & connectivity

**5.1 Wi-Fi bring-up (Wi-Fi 6 path)**
**Steps**

1. Confirm whether **ESP32-P4** uses onboard **ESP32-C6** as the Wi-Fi interface on this board; if yes, bring up the Wi-Fi stack per Waveshare/IDF guidance (C6 provides 2.4 GHz Wi-Fi 6).
2. Implement provisioning: SoftAP+Captive Portal or BLE-provisioning to collect SSID/password.

**Deliverables**: `net/wifi.c` provisioning + auto-reconnect.
**AC**: Connects to WPA2/WPA3; persists credentials; handles failures.

**5.2 TLS & trust store**
**Steps**

1. Bundle system CA set (or your broker CA).
2. Implement **certificate pinning** for MQTT and optional pinning for HTTPS fetches (GitHub Pages won’t be pinned; rely on standard CAs).

**AC**: TLS handshake succeeds; invalid certs are rejected.

---

## 6) MQTT client & protocol

**6.1 Basic client**
**Steps**

1. Use ESP-IDF `esp-mqtt` with TLS.
2. Config from NVS: `broker_host`, `port`, `client_id`, `device cert/key or token`, `topics`.

**6.2 Topics & payloads**
**Steps**

1. Subscribe to:

   * `posts/new` (global)
   * `posts/new/{artist}` (optional)
   * `device/{id}/cmd` (control)
2. Define payload schema (JSON): `{post_id, artist, playlist?, assets:[{url, sha256, w,h,format,frame_delay?}], expires_at}`.

**Deliverables**: `mqtt/client.c`, `proto/messages.h`.
**AC**: On message, it enqueues a download task; duplicate suppress.

**Security**: client certs or per-device tokens; no anonymous; subscribe-only ACLs on broker. (Matches your backend plan.)

---

## 7) Downloader & content integrity

**7.1 HTTP(S) fetcher**
**Steps**

1. Streaming GET with timeouts; limit content-size (e.g., 1.5 MB per asset).
2. Support gzip if present.

**7.2 Integrity checks**
**Steps**

1. Compute SHA256 during stream; verify equals payload’s `sha256` (and length).
2. MIME sniffer (magic bytes). Reject SVG; allow PNG/JPEG/GIF only.

**Deliverables**: `net/http_fetch.c`, `renderer/decoder_*`.
**AC**: Corrupt or wrong-hash assets are rejected; message logged.

---

## 8) Renderer & playback

**8.1 Decoders**
**Steps**

1. PNG (tinyPNGdec or lodepng), JPEG (TJpgDec), GIF (single-frame or basic animation).
2. Normalize to display format; center/pad; optional nearest-neighbor upscale for ≤128×128 art.

**8.2 Frame pipeline**
**Steps**

1. Double-buffer or partial flush; VSYNC aware if supported by DSI driver.
2. Target ≥60 FPS for simple redraws; ≥30 FPS for scroll UI.

**8.3 Playlists**
**Steps**

1. If payload indicates a playlist, sequence through assets with per-item duration; allow user “Next/Prev” via touch.

**AC**: 128×128 art displays crisp, centered on 720×720; playlist loops; no tearing.

---

## 9) UI/UX flows

**9.1 First-run wizard**

* Language, Wi-Fi, MQTT enrollment (scan QR with broker URL + device secret), brightness, time sync.

**9.2 Home screen**

* Artwork viewport; status icons; quick actions (pause, brightness, cache clear).

**9.3 Settings**

* Wi-Fi, MQTT, About (chip rev, IDF ver), storage, factory reset.

**AC**: All flows navigable by touch; long-press in corner opens Settings.

---

## 10) OTA & reliability

**10.1 OTA**

* Use ESP-IDF OTA with dual slots; server supplies signed artifact + version.
* AC: Power-loss-safe; fallback on boot failure; version shown in UI.

**10.2 Watchdogs & recovery**

* Task WDT; network task heartbeat; on repeated boot loops → safe mode (disable auto-connect, show recovery screen).

**10.3 Logs/diagnostics**

* Ring buffer; dump last N logs to SD; “Send diag” over MQTT cmd (compressed).

---

## 11) Security hardening (device)

* Enforce **TLS** everywhere; certificate pinning for MQTT.
* Store credentials in NVS **with encryption**.
* Validate all payloads against JSON schema; reject unknown fields.
* Limit URL hosts (allowlist `*.github.io`, `raw.githubusercontent.com`, etc., to avoid SSRF).
* Cache by hash; expire by `expires_at`.
* No SVG; strict PNG/JPEG/GIF sniffing.
* Rate-limit commands from `device/{id}/cmd`.

---

## 12) Performance & power

* Backlight PWM curve & max current limits.
* Idle dim after N minutes; sleep if no MQTT and no touch for M minutes; wake on touch or timer.
* Predecode next image when idle CPU available; keep RAM budget documented (PSRAM if present—check IDF support & voltage).

---

## 13) Manufacturing & provisioning

* Build a **Provisioning Tool** (Python/IDF monitor script) to flash:

  * app + bootloader + partitions
  * unique device ID
  * device certificate/key or token (from backend)
  * broker URL
* Print device label with QR (broker URI + device id).
* AC: Fresh device boots to Home within 30s after first network join.

---

## 14) Testing strategy

**Unit tests**

* Decoders (golden vectors), cache LRU, JSON parsing.

**Hardware tests**

* Wi-Fi connect/reconnect soak; MQTT subscribe/publish storm; SD hot-plug; power-cycle during OTA.

**Display tests**

* Color bars, gamma chart, touch accuracy (5-point), FPS measurement.

**Security tests**

* Invalid certs, host allowlist violations, huge payloads, wrong hashes, SVG injection.

**Benchmarks**

* Render latency (ms) 128×128→frame; memory footprint; download-to-display time.

---

## 15) CI/CD

* GitHub Actions: build for `esp32p4`, run unit tests, produce signed OTA artifacts.
* Versioning: `YY.MM.patch` + short git SHA.
* Release notes template.

---

## 16) Documentation & support

* `/docs/board-notes.md`: pin map snapshots from schematic; links to Waveshare wiki & PDF.
* `/docs/networking.md`: provisioning, MQTT topics, payload schemas.
* `/docs/ota.md`: update flow, rollback rules.
* End-user “quickstart” one-pager.

---

## 17) Execution plan (phased)

**Phase A — Bring-up (1–2 weeks)**

* Tasks: §1, §2, §3.1–3.2.
* Exit: solid colors on LCD; touch readout; UART logs.

**Phase B — Core I/O (1–2 weeks)**

* Tasks: §4, §5, §6 baseline MQTT.
* Exit: device subscribes & prints incoming post payloads.

**Phase C — Playback (1–2 weeks)**

* Tasks: §7–§8; minimal UI (§9.2).
* Exit: receives post → downloads → verifies hash → displays.

**Phase D — Polish & Resilience (1–2 weeks)**

* Tasks: §9.1, §10, §11, §12; provisioning (§13).
* Exit: OTA working, recovery safe mode, cache robust.

**Phase E — Test & Ship (1–2 weeks)**

* Tasks: §14, §15, §16.
* Exit: CI releases v1.0; test checklist green.

---

## 18) Directory layout (initial)

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

## 19) Acceptance tests (high-level)

* **AT-01**: Given Wi-Fi & MQTT creds, when a `posts/new` message arrives with a 128×128 PNG URL + sha256, device downloads, verifies, centers, and displays within **<2 s**.
* **AT-02**: If sha mismatch, device refuses to display, logs error, and waits for next post.
* **AT-03**: Power loss during OTA → on next boot device rolls back to previous version automatically.
* **AT-04**: With no network, cached last N artworks rotate locally.
* **AT-05**: Touch opens Settings; brightness persists across reboots.
* **AT-06**: Device rejects SVG or content-type mismatch.
* **AT-07**: MQTT cert expired → connection refused and user alerted.

---

## 20) Open questions / to confirm (track here)

* Exact **LCD controller** and **touch IC** part numbers (schematic/BOM). 
* **ESP32-C6** interface to P4 for Wi-Fi 6 (UART vs SDIO/SPI bridge) and the recommended Wi-Fi bring-up path for this specific board.
* Whether **PSRAM** is fitted and supported on this board/IDF combo for larger frame buffers.

---
