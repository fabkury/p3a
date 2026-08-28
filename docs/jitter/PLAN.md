# Jitter Work Stream — Plan

Companion to `README.md` (status, rules, environment). This file holds the
reasoning: what we are hunting, why we think it is what it is, how we will
prove it, and how we will fix it. Update the **Decision records** section
whenever a choice is made; never silently change an earlier decision.

## 1. Problem statement

p3a occasionally freezes an animation for ~100–500 ms and then resumes
(sometimes with a brief fast-forward). It happens on any artwork, sporadically,
under the normal workload. It is **not** the uniform slowdown seen on artworks
the chip cannot decode in time (accepted), and **not** 60 Hz quantization judder
(accepted).

Pass bar: no presented-frame lateness ≥ 100 ms over a multi-hour soak on the
normal workload. Warning tier: 50 ms.

## 2. The pipeline as it is (main @ dd8fb410)

`main/display_renderer.c`, both tasks pinned to **core 1**:

- **Producer** (`display_producer_task`, prio `CONFIG_P3A_RENDER_TASK_PRIORITY`=5):
  `acquire_free_buffer` → `animation_player_render_frame_callback` (decode one
  frame into a native buffer, upscale into the 720×720×3 back buffer; PPA for
  Giphy/museum, CPU upscale workers `upscale_top`/`upscale_bottom` prio 5 core 1
  otherwise) → overlays → `xQueueSend(g_ready_queue)` (**2 slots**).
- **Consumer** (`display_consumer_task`, prio 6): dequeue → drop stale
  generations → sleep until ~1.5 ms before `s_target_present_us` → phase-lock to
  the nearest vsync edge (binary sem given from the DPI vsync ISR, ISR stamps
  `g_last_vsync_us`) → `esp_lcd_panel_draw_bitmap` → playhead += duration.
- **Three display buffers** (1.5 MB each, PSRAM): one on glass, one pending, one
  rendering. The producer can bank at most ~1 frame of slack.
- **Resync threshold** `FRAME_TIMING_RESYNC_US` = 250 ms: a stall shorter than
  that is followed by a catch-up burst (frames presented at every vsync until
  the playhead catches up); longer stalls forfeit the time and resume at pace.
  This matches the two observed flavors (freeze+fast-forward vs freeze+resume).
- Loader (`anim_loader`, prio 4, **unpinned**) prepares the next artwork; it
  releases `s_buffer_mutex` around heavy work and signals via `s_loader_busy`,
  so mutex hold time is a probe, not a prime suspect.
- Existing `CONFIG_P3A_PERF_DEBUG` (`components/debug_http_log`) measures
  **producer** decode/upscale time only. Nothing measures presentation lateness.

## 3. Hypotheses, ranked (evidence from code + sdkconfig, 2026-08-28)

| ID | Hypothesis | Why plausible | How the trace will show it |
|----|-----------|---------------|----------------------------|
| H1 | **Flash writes stall core 1.** NVS commits (`makapix_store` ×4 sites, `config_store`, `config_store_giphy`, `app_wifi`) and LittleFS/OTA writes disable cache + halt the other core per erase/write chunk. `SPI_FLASH_AUTO_SUSPEND` is **off**; `SPI_FLASH_YIELD_DURING_ERASE=y` with 20 ms slices (still ≥1 missed frame per slice; a page rotation can sum to hundreds of ms). Whether XIP-from-PSRAM exempts core 1 on the P4 is unverified. | NVS-commit markers bracket the stall; both producer and consumer show a gap with no CPU consumed by anyone on core 1. |
| H2 | **Unpinned high-priority IDF tasks preempt the consumer on core 1.** lwIP `tcpip` (prio 18, `LWIP_TCPIP_TASK_AFFINITY_NO_AFFINITY`), esp_hosted/wifi_remote workers, MQTT. Under Wi-Fi bursts they can hold core 1 for long stretches. | Run-time-stats delta across the stall shows `tIT`/hosted tasks consuming core-1 time; stall correlates with download/MQTT markers. |
| H3 | **Long SD-card operations.** Download task (prio 3, core 0) writes + FAT metadata; loader reads next artwork; SDMMC host lock or slow-card latency blocks the producer's decode input. | Producer `decode_us` inflates only while SD-write markers are active; loader-load markers overlap. |
| H4 | **PSRAM bandwidth saturation.** DPI DMA ~93 MB/s continuous from PSRAM + code fetch from PSRAM + SD DMA + 1.5 MB memset/upscale. | Producer time inflates uniformly (decode and upscale both) during I/O bursts with no lock contention; consumer wakes on time but the frame is not ready. |
| H5 | **Blocking UART logging / log bursts** in or around the render path (115200 baud: 200 bytes ≈ 17 ms; a 10-line burst ≈ 170 ms) or a mutex held while logging. | Log-marker (bytes written by render tasks) coincides with stall; stall disappears with `CONFIG_LOG_DEFAULT_LEVEL` lowered as a provocation control. |
| H6 | **Interrupts-off windows on core 0 drop panel frames without lateness.** Found 2026-08-28: a 3–4 ms critical section (`uxTaskGetSystemState`) once per second produced a one-frame blue flash every 3–8 s (DSI vsync ISR on core 0 delayed past vblank). Release code has such windows too: flash cache-disable (H1), long `taskENTER_CRITICAL` users, `vTaskList`-style calls. | Invisible to the frame trace; only the glass shows it. Detect via a DSI underrun/vsync-miss counter if the driver exposes one, else by the `cpu0_critical` provoke and Fab's eyes. |

Deprioritized: `touch_rescue` (core 1, prio `configMAX_PRIORITIES-2`) is
boot-only and blocks on a semaphore. `s_buffer_mutex` contention (loader drops
it around heavy work). Producer overrun (out of scope; visible as uniform
slowness, filtered out by the analyzer).

## 4. Instrumentation design (Phase 1) — `CONFIG_P3A_FRAME_TRACE`

Design constraints: zero cost when the Kconfig is off (no code, no data);
negligible cost when on (a few dozen ns per frame, no allocation in the hot
path, no logging from the render tasks except the rate-limited stall report).

### 4.1 Frame ring buffer
PSRAM, `CONFIG_P3A_FRAME_TRACE_ENTRIES` (default 8192) × 32 B, single writer per
field group, lock-free (sequence counter). Entry kinds:

- **FRAME** (written by the consumer at `draw_bitmap`): `target_us`,
  `present_us`, `duration_ms`, `generation`, `queue_depth_on_dequeue`,
  `vsync_wait_us`, plus producer-side `decode_us`, `upscale_us`,
  `free_buffer_wait_us`, `produce_end_us` carried via `ready_frame_t`.
  Derived: `lateness_us = present_us − target_us`; `ready_margin_us =
  target_us − produce_end_us` (negative ⇒ producer late ⇒ overrun, out of
  scope, flagged but excluded from the pass bar).
- **MARK** (any task): `kind`, `begin/end`, `arg`. Kinds: NVS_COMMIT,
  LFS_WRITE, LOADER_LOAD, DOWNLOAD, SD_WRITE, MQTT_RX, WS_SEND, REFRESH,
  SWAP, LOG_BURST (bytes), HTTP_REQ, OTA_CHECK. Hook points are one-liners
  guarded by the Kconfig (`frame_trace_mark(kind, begin, arg)` compiles to
  nothing when off).

### 4.2 Stall detector + UART report
In the consumer, after `draw_bitmap`: if `lateness ≥ CONFIG_P3A_FRAME_TRACE_STALL_US`
(100 ms) or ≥ warn (50 ms), post to a low-priority reporter task (never print
from the consumer). Reporter emits one compact block (`JTR|...` prefixed lines
for machine parsing): the last ~2 s of FRAME/MARK entries, and, in the diag
build, the `vTaskGetRunTimeStats` delta since the previous frame (which tasks
consumed CPU on core 1 during the gap). Rate-limited to 1 report / 5 s.

### 4.3 HTTP
`GET /api/debug/frames?since=<seq>` → CSV stream of the ring (FRAME and MARK
rows), `POST /api/debug/frames/reset`, `GET /api/debug/frames/stats` → JSON
(count, p50/p99/max lateness, stalls ≥50/≥100 ms, worst-since-reset). All
behind the Kconfig; 404 otherwise.

### 4.4 Overlay
FPS overlay gains "worst lateness in last 10 s (ms)" and a red tick for 2 s
after a recorded stall, so a glance at the device confirms capture fired.

Known limitation (validated 2026-08-28): the `JTR|T` run-time delta compares
the current task list against the reporter's last periodic snapshot, so a task
created and deleted inside the window is invisible (the `cpu1` provoke hog is
one). Persistent culprits (lwIP, hosted, MQTT, download, loader) are covered.

### 4.5 Diag config overlay
`sdkconfig.defaults.diag` (used via `SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.diag"`
into a separate build dir `build-diag/`): `P3A_FRAME_TRACE=y`,
`FREERTOS_GENERATE_RUN_TIME_STATS=y`, `FREERTOS_USE_TRACE_FACILITY=y`,
`FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER=y`, `FREERTOS_VTASKLIST_INCLUDE_COREID=y`. Release `sdkconfig` untouched
(except the one-time regeneration that added the `# CONFIG_P3A_FRAME_TRACE is not set` menu lines).
The final Phase-7 soak runs with `P3A_FRAME_TRACE=y` only, on the release
config, to confirm the pass bar without the heavier scheduler.

## 5. Host tooling (Phase 2) — `host/jitter-lab/`

Python (the IDF venv has pyserial). Runs unattended, independent of any agent
session; all state on disk under `host/jitter-lab/runs/<RUN-ID>/`.

- `serial_logger.py COM5 <run>`: opens the port **without resetting** (DTR/RTS
  pre-set low; verify against `idf.py monitor --no-reset` behaviour), appends
  timestamped lines to `uart.log`, extracts `JTR|` blocks to `stalls.jsonl`,
  writes `state.json` heartbeat (pid, started, last_line_ts, stall_count).
- `pull_frames.py <run> [--every 300]`: incremental `GET /api/debug/frames?since=`
  into `frames.csv`; also snapshots `/api/state`, `/api/memory` (redacted
  `/config`) into `device/`.
- `snapshot_settings.py save|restore <run>`: playset, dwell, config (redacted
  on disk; restore uses the live values held in memory only for the session or
  re-read from the device before the run started).
- `analyze.py <run>`: lateness percentiles, stall list with attribution
  (markers overlapping [target−2 s, present]), per-hypothesis scorecard,
  excludes overrun frames (`ready_margin < 0`), renders `report.md`.
- `soak.ps1 <run> [-hours N]`: orchestrates logger + puller + periodic
  analyze; on new attributed stall → `PushNotification` (via the agent when a
  session is live; otherwise the report waits on disk).
- `find_port.ps1`: read-only probe (opening resets the board; last resort).

## 6. Execution loop (Phases 3–6)

1. **Baseline soak** (Phase 3): diag build, normal playset ("Work mix"), ≥4 h.
   Output: stall count/rate, lateness distribution, attribution table.
2. **Provocation runs** (Phase 4), one per hypothesis, ≤30 min each, on a
   synthetic 30 fps moving-bar animation (uploaded via `/upload`) so stalls are
   unambiguous and overrun is impossible:
   - H1: force an NVS commit every 10 s (dev-only `/api/debug/nvs-commit`
     endpoint, Kconfig-gated), and separately a LittleFS write.
   - H2: sustained Wi-Fi load (concurrent museum + Giphy refresh; web UI
     WebSocket hammer; large download).
   - H3: SD write burst while playing (download storm; dev-only SD stress
     writer).
   - H4: same as H3 but watch producer time inflation with SD reads only.
   - H5: log flood from an app task (dev-only endpoint) vs. log level lowered.
3. **Fix per confirmed cause** (Phase 5), each its own commit with a before/after
   run pair linked in `runs/`. Candidate fixes (choose by evidence, not by list
   order): `SPI_FLASH_AUTO_SUSPEND` (check flash chip support on the P4 module)
   and/or defer/batch NVS commits off the playback path; pin `tcpip`/hosted
   tasks to core 0 (`LWIP_TCPIP_TASK_AFFINITY_CPU0`) and audit every task ≥
   prio 6 for affinity; move hot render code to IRAM if H4 holds; remove any
   logging from render tasks; deeper decode-ahead only if it turns out to be
   needed to ride through unavoidable short stalls.
4. **Catch-up policy** (Phase 6): with real stalls rare, decide rush vs drop vs
   resync on the test animation with Fab. Fab's call, data in hand.
5. **Final soak** (Phase 7) on release config + `P3A_FRAME_TRACE=y` only; then
   report `docs/jitter/REPORT.md`; merge `feat/jitter` to main with
   `P3A_FRAME_TRACE` default n; confirm release `sdkconfig` diff is fixes-only.

## 7. Decision records

- **2026-08-28** Scope: sporadic stalls only; overrun slowness and 60 Hz
  quantization judder accepted. (Fab)
- **2026-08-28** Symptom class: stalls ~100–500 ms, sporadic, artwork-independent. (Fab)
- **2026-08-28** Pass bar: no lateness ≥100 ms in a multi-hour normal-workload soak. (Fab)
- **2026-08-28** Capture path: UART one-shot report on trigger + HTTP CSV pull;
  no SD writes by diagnostics. (Fab)
- **2026-08-28** Diag build may diverge via `sdkconfig.defaults.diag`; shipping
  sdkconfig unchanged; instrumentation shippable behind Kconfig, default off. (Fab)
- **2026-08-28** Agent may build/flash/monitor freely; may change any device
  runtime setting and restore afterwards; never erase NVS. (Fab)
- **2026-08-28** No JTAG/SystemView (USB-Serial-JTAG not wired). In-firmware
  tracing only. (Fab)
- **2026-08-28** No 240 fps glass checks until further notice. (Fab)
- **2026-08-28** Commits signed; gpg-agent cache TTL extended to 7 days. (Fab)
- **2026-08-28** Updates: chat at milestones + push notification on findings /
  blockers; LOG.md is the durable record. (Fab)
