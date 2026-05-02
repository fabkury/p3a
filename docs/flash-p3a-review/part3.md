# Part 3 — Troubleshooting and After Flashing

Covers `docs/flash-p3a.md` lines 111–138:
- Troubleshooting table (lines 111–121)
- After flashing
  - Wireless updates (OTA)
  - Adding artwork

## Verification findings

### Troubleshooting table (lines 111–119)

- **Line 115 — "No serial port found" → bad cable**: UNVERIFIABLE from the codebase (it is a generic USB/host-side issue, not something the firmware controls). Reasonable advice; no change needed.
- **Line 116 — "Failed to connect" → hold BOOT button**: CORRECT in the sense that the EP44B board has a physical BOOT button. Confirmed at `components/p3a_board_ep44b/Kconfig:64` ("EP44B has a BOOT button (active-low)…") and `components/p3a_board_ep44b/p3a_board_button.c:16,53,97`. Pressing BOOT during connect to force download mode is standard ESP32 behavior; the doc’s wording is fine.
- **Line 117 — Linux dialout group**: UNVERIFIABLE from the codebase (host OS guidance). Standard advice; no change needed.
- **Line 118 — Wrong COM port (Windows) → Device Manager**: UNVERIFIABLE (host OS guidance). No change needed.
- **Line 119 — Timeout errors → `-b 115200`**: UNVERIFIABLE from the codebase (esptool flag). The earlier flashing instructions in this same doc use `-b 460800`, so falling back to `-b 115200` is consistent. No change needed.

### Wireless updates / OTA (lines 125–130)

- **Line 128 — `http://p3a.local/ota`**: CORRECT.
  - Route registered at `components/http_api/http_api_ota.c:368` (`GET /ota` → serves `/webui/ota.html` at line 372).
  - mDNS hostname `p3a.local` is consistent with `CLAUDE.md` (`wifi_manager` section).
  - Page exists at `webui/ota.html`.
- **Line 129 — Click "Check for updates"**: INCORRECT (label casing). The actual button label in `webui/ota.html:173` is **"Check for Updates"** (capital U). Minor; suggested fix: render as `"Check for Updates"` to match the UI exactly. The page also shows a separate "Web UI" card with its own `Install Web UI Update` button (`webui/ota.html:169`), which the guide does not mention — acceptable for a quickstart, but note that on first OTA the user may need to update the Web UI separately as the page itself instructs (`webui/ota.html:175`).
- **Line 130 — Click "Install" if an update is available**: INCORRECT (label). The actual firmware install button text is **"Install Update"** (`webui/ota.html:154`). Suggested fix: `Click "Install Update" if an update is available`.

### Adding artwork (line 134)

- **"Copy WebP, GIF, PNG, or JPEG files to an `animations` folder on a microSD card"**: PARTIALLY CORRECT.
  - Supported formats CORRECT: `.webp`, `.gif`, `.png`, `.jpg`/`.jpeg` are all matched (case-insensitive) at `main/animation_player_loader.c:460-464`. Decoders exist at `components/animation_decoder/{webp,png,jpeg}_animation_decoder.c` and `components/animated_gif_decoder/`.
  - Folder location is INCOMPLETE / MISLEADING. The canonical, configured path is **`/sdcard/p3a/animations/`** (i.e. an `animations` folder inside a top-level `p3a` folder), not an `animations` folder at the SD card root:
    - `components/p3a_core/sd_path.c:89-92` — `sd_path_get_animations()` resolves to `<root>/animations` where `<root>` is the `p3a` subdirectory on the SD card (per `sd_path_get_subdir`).
    - `CLAUDE.md` Storage Layout: `/sdcard/p3a/animations/` — Local files.
    - `README.md:212` — `/sdcard/p3a/animations/` (local files).
    - `components/play_scheduler/play_scheduler_cache.c:8` and `play_scheduler_refresh.c:861` both reference `/sdcard/p3a/animations`.
  - There is a recursive fallback `find_animations_directory()` (`main/animation_player_loader.c:475`) that walks the SD card looking for any directory containing animation files, so a bare `animations/` folder at the SD card root will likely also work. However, the documented/canonical location is `p3a/animations/`, so the guide should reflect that to avoid confusion with future Makapix vault / Giphy cache siblings.
  - Suggested fix:
    > Copy WebP, GIF, PNG, or JPEG files to a `p3a/animations/` folder on a microSD card (i.e. create a `p3a` folder at the root of the card, and an `animations` folder inside it), then insert it into your p3a.

### Pointer to HOW-TO-USE.md (line 136)

- **`docs/HOW-TO-USE.md` exists**: CORRECT. Confirmed via Glob at `docs/HOW-TO-USE.md`. Relative link `HOW-TO-USE.md` from `docs/flash-p3a.md` resolves correctly.

### Summary of recommended edits to `docs/flash-p3a.md`

1. Line 129: `"Check for updates"` → `"Check for Updates"` (match UI casing).
2. Line 130: `"Install"` → `"Install Update"` (match UI label).
3. Line 134: clarify the artwork folder is `p3a/animations/` (i.e. inside a top-level `p3a` directory), not a bare `animations` folder at the SD root.
4. Optional: add a one-line note that the OTA page also offers a separate "Install Web UI Update" button on the Web UI card, and that on first OTA both may need to be applied (firmware first, then Web UI), per the in-page tip at `webui/ota.html:175`.
