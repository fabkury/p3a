# Part 2 — Command Line Flashing

Covers `docs/flash-p3a.md` lines 40–109:
- Option 3: Command Line (All Platforms)
  - Step 1: Install Python and esptool
  - Step 2: Download firmware
  - Step 3: Connect the board
  - Step 4: Flash (Windows PowerShell, Windows cmd, macOS/Linux)
  - Step 5: Done!

## Verification findings

### Step 1 — Install Python and esptool (lines 44–64)

- **Windows / macOS / Linux install instructions**: UNVERIFIABLE from the codebase
  (these are generic third-party instructions for python.org, Homebrew, and apt).
  They are conventional and commonly correct, but nothing in this repo pins them.
  No action required unless the project wants to recommend a minimum esptool
  version (modern esptool ≥ 4.6 is needed for `--chip esp32p4`; the doc does
  not state a minimum).

### Step 2 — Download firmware (lines 66–70)

- **Release URL `github.com/fabkury/p3a/releases`** and **"firmware .zip"**:
  UNVERIFIABLE from the codebase alone (no release-publishing script in-tree
  pins the URL). The repo does produce per-version artifacts under
  `release/v*/` (e.g. `release/v0.9.0/flash_args`, plus the matching `.bin`
  files), so a zip of that folder is a plausible artifact shape. Suggest
  the doc spell out which files the user should expect inside the zip
  (`p3a.bin`, `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin`,
  `storage.bin`, `network_adapter.bin`, `flash_args`) — they match
  `release/v0.9.0/flash_args` exactly.

### Step 3 — Connect the board (lines 72–79)

- **"USB-C data cable" required**: CORRECT in spirit — consistent with
  `docs/HOW-TO-USE.md:27` ("USB-C data cable (not a charging-only cable)").
- **"Full-Speed (FS) USB-C port"**: LIKELY CORRECT but the codebase only
  weakly substantiates it.
  - The board has **two USB-C ports**, confirmed by
    `docs/HOW-TO-USE.md:200` ("p3a has two USB-C ports, but only one (the
    High-Speed port) works as a USB storage device").
  - `docs/BOARD-CAPABILITIES.md:251–252` lists the two ports as
    **"USB 2.0 HS OTG"** (480 Mbps Type-C) and **"USB-to-UART"** (Type-C,
    "for flashing and debug serial").
  - The "FS" label in `flash-p3a.md:75` is therefore a reference to the
    USB-to-UART (CP210x/CH340-style serial bridge) port, not to a
    USB-spec Full-Speed peripheral. Strictly speaking the bridge enumerates
    as a UART and is not a USB-FS device on the ESP32-P4. Recommend
    rewording to **"the UART (flashing) USB-C port — the one labelled
    UART/Serial on the silkscreen, not the HS port used for USB storage"**
    to match `BOARD-CAPABILITIES.md` and `HOW-TO-USE.md` terminology.
- **Serial port discovery commands** (`Device Manager`, `ls /dev/cu.usb*`,
  `ls /dev/ttyUSB*`/`/dev/ttyACM*`): UNVERIFIABLE from the codebase but
  conventional and correct for the typical CP210x/CH340 bridge.

### Step 4 — Flash command (lines 81–100)

Command under audit:
`--chip esp32p4 -p <PORT> -b 460800 --before default_reset --after hard_reset write_flash --flash-mode dio --flash-freq 80m --flash-size 32MB --force @flash_args`

- **`--chip esp32p4`**: CORRECT.
  Build target is ESP32-P4 (project sets `idf.py set-target esp32p4`,
  matches `CLAUDE.md` and `sdkconfig` chip target).
- **`-p` / `-b`**: CORRECT.
  Modern esptool short flags; equivalent to `--port` / `--baud`.
- **`-b 460800`**: CORRECT (UNVERIFIABLE specifically from the repo, but a
  standard, safe baud for ESP32-P4 over the on-board USB-UART bridge).
- **`--before default_reset --after hard_reset`**: CORRECT (esptool defaults,
  appropriate for boards with auto-reset circuitry like this one).
- **`--flash-mode dio`**: CORRECT.
  Confirmed by `build/flasher_args.json` (`"flash_mode": "dio"`,
  `"--flash_mode", "dio"`) and `build/flash_args` line 1
  (`--flash_mode dio --flash_freq 80m --flash_size 32MB`).
  Note: `sdkconfig` lines 712–717 show `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y`
  but `CONFIG_ESPTOOLPY_FLASHMODE="dio"`; ESP-IDF deliberately downgrades
  QIO→DIO at flash-write time (because the second-stage bootloader sets
  the real mode), which is why the generated `flash_args` is `dio`. The
  doc's `dio` matches what the build system actually emits, so it is
  correct as-is.
- **`--flash-freq 80m`**: CORRECT.
  `sdkconfig:719` `CONFIG_ESPTOOLPY_FLASHFREQ_80M=y`,
  `sdkconfig:723` `CONFIG_ESPTOOLPY_FLASHFREQ="80m"`,
  matched in `build/flash_args` and `build/flasher_args.json`.
- **`--flash-size 32MB`**: CORRECT.
  `sdkconfig:729` `CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y`,
  `sdkconfig:732` `CONFIG_ESPTOOLPY_FLASHSIZE="32MB"`, matched in
  `build/flash_args`.
- **`--force`**: PROBABLY UNNECESSARY.
  `--force` bypasses chip/flash-size sanity checks. The build flashes a
  32 MB image on a 32 MB part with the matched `--chip esp32p4`, so the
  checks should pass without it. It is harmless but the doc could drop
  it to avoid suggesting users routinely override safety checks. If
  retained, a one-line note explaining *why* (e.g. some pre-prod boards
  report the wrong flash ID) would be helpful.
- **`@flash_args` / `"@flash_args"` quoting**: CORRECT.
  - In PowerShell `@` is the splatting/here-string sigil, so quoting the
    argument as `"@flash_args"` (line 87) is the right way to pass it
    through to esptool literally.
  - In cmd.exe and POSIX shells the bare `@flash_args` (lines 92, 97)
    works without quoting. Both forms are accurate.
  - Matches the file actually shipped in `release/v0.9.0/flash_args`
    (offsets `0x2000 bootloader.bin`, `0x8000 partition-table.bin`,
    `0x10000 ota_data_initial.bin`, `0x20000 p3a.bin`,
    `0x1020000 storage.bin`, `0x1420000 network_adapter.bin`).
    Note the release `flash_args` lists files by basename in the same
    directory, so users running the command from inside the extracted
    `p3a-firmware` folder will resolve them correctly — that matches
    Step 4's instruction to "open a terminal in the firmware folder".

- **Minor wording**: "~2 minutes for flashing" (line 100) is plausible
  for ~24 MB total at 460800 baud over USB-UART; UNVERIFIABLE from the
  codebase but consistent.

### Step 5 — Done! (lines 102–107)

- **Splash screen on reboot**: UNVERIFIABLE in detail from grep alone, but
  consistent with `main/ugfx_ui.c` rendering the setup screen (which
  shows `CONFIG_ESP_AP_SSID` per `main/ugfx_ui.c:168`). Treat as
  CORRECT.
- **`p3a-setup` Wi-Fi network**: CORRECT.
  - `main/Kconfig.projbuild:193–195` defines `ESP_AP_SSID` with
    `default "p3a-setup"`.
  - `sdkconfig:812` `CONFIG_ESP_AP_SSID="p3a-setup"`.
  - `components/wifi_manager/wifi_captive_portal.c:29,767,794` and
    `components/wifi_manager/app_wifi.c:50` use that value as the
    soft-AP SSID.
- **`http://p3a.local/`**: CORRECT (mDNS hostname is documented in
  `CLAUDE.md` under `wifi_manager` — "mDNS (`p3a.local`)"). Not
  re-grepped here, but consistent with the project description.

### Summary

| Claim | Status |
|---|---|
| Generic Python/esptool install steps | UNVERIFIABLE (conventional, OK) |
| GitHub releases zip → extract to `p3a-firmware` | UNVERIFIABLE (plausible) |
| Two USB-C ports on board | CORRECT |
| "Full-Speed (FS)" port = flashing port | LIKELY CORRECT but mis-labelled — the codebase calls it the **UART** port (HS is the storage port). Recommend rewording. |
| `--chip esp32p4` | CORRECT |
| `-p`/`-b` flags | CORRECT |
| `--flash-mode dio` | CORRECT (matches `build/flash_args`, `flasher_args.json`) |
| `--flash-freq 80m` | CORRECT (`CONFIG_ESPTOOLPY_FLASHFREQ_80M=y`) |
| `--flash-size 32MB` | CORRECT (`CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y`) |
| `--force` | PROBABLY UNNECESSARY; harmless but should be justified or dropped |
| `@flash_args` (and `"@flash_args"` in PowerShell) | CORRECT |
| `p3a-setup` SSID | CORRECT (`CONFIG_ESP_AP_SSID="p3a-setup"`) |
| `http://p3a.local/` | CORRECT |

### Suggested rewording

- Line 75 — replace
  `Connect to the **Full-Speed (FS) USB-C port** on your board`
  with
  `Connect to the **UART USB-C port** on your board (the one labelled
  UART/Serial — *not* the HS port used for USB storage access).`
- Lines 87/92/97 — consider dropping `--force` unless there is a known
  reason it is required for these boards; if it is required for some
  hardware revisions, add a short footnote explaining when.
- Line 70 — optional: list the files the user should see after
  extracting the zip (`p3a.bin`, `bootloader.bin`,
  `partition-table.bin`, `ota_data_initial.bin`, `storage.bin`,
  `network_adapter.bin`, `flash_args`) so users notice if a download
  is incomplete before flashing.
