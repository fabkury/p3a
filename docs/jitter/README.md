# Jitter Work Stream — START HERE

Goal: eliminate **sporadic playback stalls** (100–500 ms freezes, not tied to any
artwork) on p3a, measured and proven on hardware, with **zero overhead in release
builds**. Multi-week effort designed to be interrupted and resumed by a fresh
AI-agent session at any time.

If you are a new session picking this up: read this file top to bottom, then
`LOG.md` (last 3 entries), then the phase's section in `PLAN.md`. Do not start
work before checking **Current status** and **Resume protocol** below.

## Current status

**Phase: 1+2 done (2026-08-28), device-validated. Next: Phase 3 — baseline soak.**

| Phase | Description | Status |
|-------|-------------|--------|
| 0 | Foundation: branch, docs, device recon, signing, tooling skeleton | done 2026-08-28 |
| 1 | Firmware instrumentation (`CONFIG_P3A_FRAME_TRACE`): frame ring buffer, event markers, stall detector w/ UART report, `/api/debug/frames` CSV, overlay tick | done 2026-08-28 — `components/frame_trace`, validated on device (forced core-1 hog → 177 ms stall detected, reported, attributed) |
| 2 | Host tooling `host/jitter-lab/`: persistent serial logger, HTTP puller, analyzer, run archiver | done 2026-08-28 — reset-free logger verified; `soak.ps1` orchestrates |
| 3 | Baseline soak on normal workload (multi-hour); attribution table | not started |
| 4 | Provocation runs per hypothesis (H1..H5 in PLAN.md) | not started |
| 5 | Fixes, one per confirmed cause, each with before/after soak | not started |
| 6 | Catch-up policy decision (rush / drop / resync) with data | not started |
| 7 | Final soak on plain build (trace on, no diag overlay) against pass bar; report; merge | not started |

Pass criterion (Fab, 2026-08-28): **no presented-frame lateness ≥ 100 ms during a
multi-hour soak on the normal workload** (Makapix + Giphy/Klipy + museum channels
rotating, Wi-Fi up). Single missed vsyncs and quantization judder are accepted.
Uniform slow playback of artworks the chip cannot decode in time is accepted.

## Ground rules (decided with Fab 2026-08-28, do not re-litigate)

- **Release builds carry no overhead.** All instrumentation is behind
  `CONFIG_P3A_FRAME_TRACE` (default n). Heavier probes (FreeRTOS run-time stats,
  trace facility) live only in `sdkconfig.defaults.diag` (Phase 1). Release
  `sdkconfig` must stay byte-identical except for fixes that are themselves the
  deliverable. Any sdkconfig regeneration must preserve
  `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` + `CONFIG_ESP32P4_REV_MIN_1=y`
  (see root CLAUDE.md).
- **Automation first.** The agent builds, flashes, monitors, runs soaks, pulls
  data, and analyzes on its own. Fab is consulted only for decisions or physical
  actions. Keep Fab informed: chat summary at each phase milestone; push
  notification when a soak yields an attributed stall, a fix is ready for his
  eyes, or work is blocked. Everything also goes into `LOG.md`.
- **Device is fair game, restore afterwards.** Any runtime setting, playset,
  upload, reboot or flash is allowed on the dev unit; snapshot settings before a
  run and restore after. Never `erase-flash`, never touch NVS contents
  (Wi-Fi, Makapix registration, API keys live there).
- **No high-speed camera glass checks** until Fab says otherwise.
- **Commits are GPG-signed** on `feat/jitter` (Fab extended the gpg-agent cache
  TTL to 7 days on 2026-08-28; commit with the sandbox off). Commit small and
  often; every commit message says which phase it belongs to.
- **Never put API keys in docs or run archives.** `GET /config` returns Giphy,
  Klipy, HAM and SI keys in the clear; tooling redacts `*_api_key` fields.
- Scope guard: producer overrun (slow decode) and 60 Hz quantization judder are
  explicitly out of scope. Do not "fix" them while here.

## Environment facts (verified 2026-08-28)

| Item | Value |
|------|-------|
| Dev device UART | **COM5** (CH343 bridge, 115200). CLAUDE.md's "COM11" is stale for this unit. Probe: `host/jitter-lab/find_port.ps1` |
| Opening COM5 with .NET `SerialPort` or plain `idf.py monitor` **resets the board**. `host/jitter-lab/serial_logger.py` (pyserial, DTR/RTS low before open) does **not** (verified). `idf.py flash` needs COM5 free: stop the logger first |
| USB-Serial-JTAG | not wired to the laptop; no OpenOCD/SystemView. In-firmware tracing only |
| Device LAN name | `http://p3a-fab.local` (device name "fab", hostname `p3a-fab`; IP 192.168.4.87 on 2026-08-28). **`p3a.local` is a DIFFERENT, unrelated p3a on this LAN** (192.168.4.33): tooling refuses any host whose `/api/device-name` hostname is not `p3a-fab` |
| SD root | `sdcard_root = /p3a2` (not the default `/p3a`); all SD paths resolve via `sd_path` |
| Firmware baseline | main @ `dd8fb410`, version in root `CMakeLists.txt` |
| Panel | 720×720 @ 60 Hz (`VSYNC_PERIOD_US 16667` in `main/display_renderer.c`) |
| CPU | ESP32-P4 rev v1.0 @ 360 MHz, PSRAM 200 MHz, XIP from PSRAM (`SPIRAM_FETCH_INSTRUCTIONS` + `SPIRAM_RODATA`) |
| Build env | see root CLAUDE.md (IDF 5.5.4 profile, `ESP_IDF_VERSION=5.5`, `PYTHONUTF8=1`); always `Set-Location` repo root in the same command as `idf.py` |
| Useful API | `GET /api/state`, `GET /api/memory`, `GET /config` (redact!), `PUT /config`, `GET/PUT /playsets/active`, `PUT /settings/dwell_time`, `POST /action/{swap_next,pause,resume,reboot}`, `POST /upload` (multipart → animations dir) |

## Layout

```
docs/jitter/
  README.md     this file: status, rules, environment, resume protocol
  PLAN.md       goal, hypotheses (H1..H5) with evidence, instrumentation design,
                execution loop, fix strategy, decision records
  LOG.md        append-only dated journal of what was done and learned
  runs/         one committed summary per soak/provocation run (RUN-YYYYMMDD-NN.md)
host/jitter-lab/
  README.md     tooling usage + endpoint/report formats
  build.ps1     release / diag build with guards; -Flash
  soak.ps1      start/status/stop a detached soak (logger + puller), -NewRunId
  serial_logger.py, pull_frames.py, snapshot_settings.py, analyze.py
  find_port.ps1 serial port probe (read-only, note: opening resets the board)
  runs/         raw data per run (gitignored; large CSVs + UART logs)
components/frame_trace/   the instrumentation (Kconfig-gated, default off)
components/http_api/http_api_rest_debug_frames.c   /api/debug/* endpoints
sdkconfig.diag.defaults   diag overlay (trace on, run-time stats, dev endpoints)
```

## Resume protocol (fresh session)

1. `git status`, `git log --oneline -5`, confirm branch `feat/jitter`.
2. Read `LOG.md` tail. The last entry ends with a **Next** line; that is the
   next action. If it says a soak/run is in progress, check
   `host/jitter-lab/runs/<id>/` for a live logger (`state.json`) before starting
   anything that touches the device.
3. Check the device: `curl http://p3a-fab.local/api/state`. If unreachable, check
   COM5 is free (a logger may hold it) before opening it, since opening resets
   the board.
4. Do the next action. Append a `LOG.md` entry when you stop, always ending with
   **Next:**. Update the status table above when a phase changes state.
5. Commit (signed) before ending a session. Uncommitted work is lost work.

## Interruption protocol

Stopping is always safe if: LOG.md has an entry with a **Next** line, the work
tree is committed, and any running host tooling either keeps running unattended
(it is designed to) or has been stopped and its run summarized in `runs/`.
