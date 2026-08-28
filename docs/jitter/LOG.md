# Jitter Work Stream — Log

Append-only, chronological (newest at the bottom). Every entry ends with a
**Next:** line. Keep entries factual: what was done, what was learned, what
changed in the plan. Link runs as `runs/RUN-YYYYMMDD-NN.md`.

---

## 2026-08-28 — Phase 0: foundation

Context: Fab reports sporadic 100–500 ms stalls, artwork-independent, on the
normal workload. Uniform slow playback (decode overrun) and 60 Hz quantization
judder are explicitly accepted. Full decision set in `PLAN.md` §7.

Done:
- Read the render pipeline (`main/display_renderer.c` producer/consumer,
  `animation_player_render.c`, loader). Findings recorded in `PLAN.md` §2.
- Ranked hypotheses H1..H5 (`PLAN.md` §3). Leading: flash-write core stalls
  (H1) and unpinned prio-18 lwIP task on core 1 (H2).
- Device recon: dev unit is on **COM5** (CLAUDE.md's COM11 is stale for it);
  **opening the port with .NET SerialPort resets the board** (confirmed:
  uptime 35 s right after the probe). `p3a.local` resolves, REST API answers.
  `sdcard_root=/p3a2`. `GET /config` leaks API keys → redact everywhere.
- Fab pointed out a no-reset monitor option; to verify in Phase 2:
  `idf.py monitor --no-reset` and pyserial open with DTR/RTS pre-set low.
- Branch `feat/jitter` created from main @ dd8fb410. GPG signing verified
  unattended (cache TTL 7 days).
- Wrote `README.md` (status/rules/env/resume protocol), `PLAN.md`,
  this log, `host/jitter-lab/` skeleton (README + `find_port.ps1`),
  `.gitignore` for raw run data.

Learned:
- Existing `CONFIG_P3A_PERF_DEBUG` measures producer time only; presentation
  lateness is not measured anywhere today.
- `FRAME_TIMING_RESYNC_US` = 250 ms explains the two observed stall flavors.
- `SPI_FLASH_AUTO_SUSPEND` is off; `SPI_FLASH_YIELD_DURING_ERASE=y` (20 ms).
- `LWIP_TCPIP_TASK_AFFINITY_NO_AFFINITY=y`; `ESP_TIMER` and main task on core 0.

**Next:** Phase 1 — implement `CONFIG_P3A_FRAME_TRACE` per `PLAN.md` §4
(ring buffer + marks + stall detector/reporter + `/api/debug/frames*` + overlay
tick + `sdkconfig.defaults.diag`), build both configs, flash the diag build to
COM5, confirm the ring fills and a forced stall (dev endpoint) triggers a `JTR|`
report on UART.

## 2026-08-28 — Phase 1 + 2: instrumentation and host tooling, device-validated

Done:
- `components/frame_trace/` (Kconfig `P3A_FRAME_TRACE`, default n; CMake compiles
  no sources when off; header macros vanish). 56-byte entries, lock-free
  multi-writer PSRAM ring (8192 entries), FRAME records from the consumer,
  MARK records from any task, stats + histogram, trailing-10 s worst, stall
  detector → reporter task (core 0, prio 2) printing `JTR|` blocks with the
  2 s history and a `uxTaskGetSystemState` run-time delta; `esp_log_set_vprintf`
  hook marks any single log call slower than 2 ms (`log_slow`).
- Hooks: consumer/producer in `display_renderer.c` (target captured BEFORE
  baseline/resync so resynced frames keep their true lateness; flags
  BASELINED/RESYNCED/MAX_SPEED/UI/BLACK), decode/upscale split in
  `animation_player_render.c`, marks for NVS commits (8 sites), loader load,
  download, MQTT rx, refresh (3 sites), swap (generation bump), mode switch,
  resync, HTTP requests. Overlay: yellow worst-10 s ms + red stall block under
  the FPS counter (show_fps-gated).
- `/api/debug/frames` CSV, `/frames/stats` JSON, `/frames/reset`, `/mark`,
  dev-only `/provoke?kind=nvs|log|sd|cpu1` (`components/http_api/http_api_rest_debug_frames.c`).
- `sdkconfig.diag.defaults` overlay + `host/jitter-lab/build.ps1` (guards:
  release sdkconfig unchanged, P4 rev lines present, trace on/off as expected).
  Release map has zero frame_trace / debug_frames objects. One-time accepted
  change to the tracked `sdkconfig`: the `# CONFIG_P3A_FRAME_TRACE is not set`
  menu lines.
- Host tooling: `serial_logger.py` (reset-free, verified: uptime 40.9 s → 47.6 s
  across a logger start), `pull_frames.py`, `snapshot_settings.py`, `analyze.py`
  (per-generation median producer time separates decode overrun from starvation),
  `soak.ps1` (detached processes, pids.json).

Validation run `RUN-20260828-00-validate` (diag build, normal playset):
- `cpu1` provoke (300 ms hog at prio 7 on core 1) → 1 hard stall, lateness
  177 ms, `free_wait` 118 ms, UART `JTR|STALL` report fired within 1 s,
  analyzer attributed it to the `provoke` interval. Detector + report + pull +
  analysis chain works end to end.
- `nvs` ×3 (1 KB blob commits): no lateness change at all (max stayed 177 ms,
  worst-10 s 21 ms). H1 is weaker than assumed for small commits; a page-rotation
  provocation (n=40+) is still owed in Phase 4.
- `log` ×30 (200-byte INFO lines from httpd): overrun count 1 → 6, worst-10 s
  54 ms. Log floods delay the PRODUCER (H5 evidence, producer side).
- `sd` 8 × 64 KB writes: no effect.
- Baseline (no provocation) on this artwork mix: lateness p50 0.2 ms, p90 7 ms,
  p99 8.5 ms (the 5–17 ms bucket is vsync-edge alignment, expected).
- First `JTR|T` lines showed `core=-1`: `xCoreID` needs
  `CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID=y` (added to the diag overlay).
  Short-lived tasks are invisible to the delta (documented in PLAN §4.4).

**Next:** confirm core ids in `JTR|T` after the reflash, commit, then Phase 3:
`soak.ps1 -Start` on the normal playset ("Work mix") for ≥ 4 h with show_fps
left as is, analyze, write `docs/jitter/runs/RUN-….md`, push-notify Fab if any
in-scope stall is attributed.

## 2026-08-28 — Phase 3 started: baseline soak RUN-20260828-01

- Reflashed diag with `FREERTOS_VTASKLIST_INCLUDE_COREID=y` (needed
  `build.ps1` to learn that IDF applies `SDKCONFIG_DEFAULTS` only when the
  generated sdkconfig is created; it now regenerates `build-diag/sdkconfig`
  whenever a source is newer). `JTR|T` now shows affinity: 0/1 pinned,
  `any` (tskNO_AFFINITY) unpinned.
- New H2 evidence from the task list: `sdio_read` / `sdio_write` (esp_hosted
  transport) run at **prio 23, unpinned**; `tiT` (lwIP) prio 18 unpinned;
  `download_mgr` was observed at prio 23 (priority inheritance from the SDIO
  bus mutex, presumably) — it is pinned to core 0 so not a core-1 preemptor,
  but the sdio tasks can land on core 1 above the consumer.
- mDNS `p3a.local` was unresolvable for ~60 s after a reboot while the IP
  (`192.168.4.87`) answered; tooling keeps `p3a.local` (puller retries) but use
  the IP right after a flash.
- Soak started 13:48 local: `soak.ps1 -Start -Run RUN-20260828-01 -Hours 8`,
  normal playset "Work mix", settings snapshot saved, stats reset at start.
  Logger pid 31792, puller pid 13396 (see `runs/RUN-20260828-01/pids.json`).

**Next:** let the soak run (≥ 4 h; puller stops itself after 8 h, logger runs
until stopped). Then `soak.ps1 -Stop -Run RUN-20260828-01` → `report.md`,
write `docs/jitter/runs/RUN-20260828-01.md`, update README status, push-notify
Fab with the stall count and top attribution, commit. Commit the pending
cosmetic fix (`core=any` printing) with it.

## 2026-08-28 — RUN-01 aborted after 5 min; reporter gating fixed; RUN-02 started

- RUN-20260828-01 (see `runs/RUN-20260828-01.md`): a chronically slow artwork
  drove lateness into the 250 ms resync every ~5.6 s; the reporter treated every
  resync as a stall and flooded UART. The analyzer side was already right
  (887 overrun vs 2 in-scope, both the pre-soak provoke).
- Firmware: `frame_trace_frame` now keeps an EMA of producer time per
  generation; lateness that the producer explains (margin < 0, produce_us in
  line with the EMA) increments `stalls_overrun` and does NOT notify the
  reporter. Starved producers (produce_us >= 3x EMA or >= EMA + one frame) and
  consumer-side stalls still report. Stats JSON exposes `stalls_overrun`.
- Tooling: `pull_frames.py --from-head` (soak starts at the ring head);
  `soak.ps1 -Start` resets stats first.
- Phase 6 note recorded in the run summary: resync policy vs chronic overrun.

- Two build slips on the way (a line-based patch split a string literal; the
  Bash tool de-escapes `\n` inside quoted heredocs, so write escapes as
  `chr(92)+'n'` or use the Write tool). `build.ps1` piped through
  `Select-String` masks its exit code: always look for `BUILD OK`/`FLASH OK`.
- RUN-20260828-02 started 13:59 local on diag cc351650 (reporter gating,
  `stalls_overrun` in stats), puller from ring head seq 695, `-Hours 8`.
  Logger pid 40048, puller pid 31268.

**Next:** let RUN-02 run ≥ 4 h; then `soak.ps1 -Stop -Run RUN-20260828-02`,
analyze, write `docs/jitter/runs/RUN-20260828-02.md`, update README status,
push-notify Fab, commit.

## 2026-08-28 — RUN-02 aborted (gating rule), RUN-03 started

- RUN-02 reported a producer-explained 293 ms frame on a 40 ms artwork whose
  producer time swings 80–137 ms (`runs/RUN-20260828-02.md`). Anomaly rule is
  now `produce_us >= 2x EMA` only. Rebuilt + flashed.
- RUN-20260828-03 started ~14:14 local, same parameters as RUN-02.

**Next:** let RUN-03 run ≥ 4 h; stop, analyze, summarize, notify Fab, commit.

## 2026-08-28 — Blue-flash glitch caused by the diag build; RUN-03 aborted

- Fab: one-frame blue flash every 3–8 s on every animation since today's
  diag builds. Root cause and fix in `runs/RUN-20260828-03.md`: the periodic
  `uxTaskGetSystemState()` (3–4 ms interrupts-off on core 0) delayed the DSI
  vsync ISR. Replaced by a cheap per-task `vTaskGetInfo` poll; full snapshots
  only at init, on task-count change, and at report time (all marked).
- Reflashed. Awaiting Fab's confirmation that the flashing is gone before
  starting RUN-04. If it persists, next suspects: FreeRTOS run-time-stats /
  trace-facility overhead per context switch (diag overlay), then the log hook.
- New hypothesis H6 for PLAN §3: interrupts-off windows on core 0 (critical
  sections, flash cache-disable) drop panel frames without producing lateness;
  the frame trace cannot see them, only the glass can.

**Next:** get Fab's verdict on the flashing; if gone, start RUN-20260828-04
(same parameters) and monitor; add H6 to PLAN §3 and a `cpu0_critical`
provoke kind in Phase 4.

## 2026-08-28 — RUN-04: first genuine stalls; probes added; RUN-05

- RUN-04 (5 min, `runs/RUN-20260828-04.md`): 12 in-scope stalls. Signature is
  a single-frame upscale blow-up (14 → 50–280 ms) in bursts of 15–30 s every
  ~70 s, artwork-independent; both upscale workers slow rather than preempted;
  core 0 saturated by download_mgr without a download (verify sweep). SD card
  logged write failures at the same time. Leading hypotheses now H3/H4.
- Probes for RUN-05: `--wrap` of sdmmc sector I/O and esp_flash ops (diag
  only, `frame_trace_wraps.c`), `verify` marks, analyzer anomaly section.
- Reflashed 14:2x; RUN-20260828-05 started.

**Next:** analyze RUN-05 after ≥ 30 min against the new marks (sd_read /
sd_write / flash_op / verify lift on producer anomalies). If SD I/O is the
driver, Phase 4 provoke = `sd` read storm + verify sweep on the moving-bar
animation; Phase 5 candidates: move upscale_top off core 0, throttle the verify
sweep, check SD host/DMA cache behaviour.

## 2026-08-28 — RUN-05: SD writes implicated; ring overflow fixed; RUN-06

- RUN-05 (`runs/RUN-20260828-05.md`): every in-scope stall had an `sd_write`
  in its window; 32 KB SD writes take 72 ms median / 345 ms p99 (sector pace);
  flash ops are all cheap 32-byte NVS reads (H1 out). sd_read lift 2.3x,
  sd_write 1.5x on producer anomalies.
- Ring overflowed at 70 marks/s: RUN-06 uses a 32768-entry ring and span marks
  (one entry per SD/flash op with its duration). Analyzer: bisect-based
  coverage (was O(n²), timed out), span-mark pairing, seq dedupe.
- Phase 4 design sharpened: provoke SD writes from PSRAM vs internal buffers,
  512 B vs 32 KB, on the moving-bar animation; expect upscale anomalies to
  track the PSRAM-buffer case if H3/H4 holds.

**Next:** RUN-06 ≥ 1 h with span marks; then Phase 4 SD provocations; then
fix candidates (SD write path: multi-block writes / internal bounce buffer /
lower SD clock check; upscale_top off core 0).

## 2026-08-28 — H3b: SD DMA bounce path; candidate fix committed (untested); RUN-06 running

- Mechanism (IDF 5.5.4 `sdmmc_cmd.c` + `sdmmc_host.c`): on the P4 the SD host
  DMAs directly to/from PSRAM only if address and size are 128-byte aligned;
  otherwise 512 B per SD command via an internal bounce buffer. `http_fetch`
  chunk and `loader_service` file buffer are unaligned PSRAM mallocs → the
  slow writes and the 512 B read storms seen in RUN-05.
- Candidate fix `065d6c90` (aligned allocs in both places) is committed but
  NOT built or flashed: RUN-06 (started 14:42, diag 6d869d30, unfixed) is the
  "before"; RUN-07 with the fix is the "after". Keep RUN-06 ≥ 1 h.
- Still open: why a slow SD write inflates the CPU upscale on both cores
  (H4-style contention) rather than merely being slow. If the fix removes the
  stalls the question becomes academic; if not, Phase 4 provocations
  (PSRAM-vs-internal buffers, 512 B vs 32 KB) follow.

**Next:** at ~15:45 stop RUN-06 (`soak.ps1 -Stop`), analyze, write
`runs/RUN-20260828-06.md`; build+flash diag with `065d6c90`, start RUN-07,
compare stall rate, sd_write/sd_read durations and counts, upscale anomalies.

## 2026-08-28 — Blue flashes confirmed gone (Fab)

- Fab confirms the one-frame blue flashes are gone on diag builds ≥ f1134ce4
  (no periodic `uxTaskGetSystemState`). H6 (interrupts-off windows on core 0
  drop panel frames) stays in PLAN as a verified mechanism; the trace cannot
  see it, so any future critical-section audit needs the glass as its judge.

## 2026-08-28 — RUN-06 (before) closed; RUN-07 (after, aligned SD buffers) started

- RUN-06 (`runs/RUN-20260828-06.md`): 32 stalls in 0.85 h (37.7/h), 280
  producer anomalies (274 upscale), every stall with an sd_write in window;
  11 810 SD writes at 70 ms median, 27 % of wall time inside SD writes.
- Flashed diag with 065d6c90 (http_fetch chunk + loader_service buffer
  128-byte aligned → direct SD DMA). RUN-20260828-07 started 15:50, same
  playset, 30 s pulls, monitor armed (also reports reboots via boots.jsonl).
- Compare after ≥ 1 h: stall rate, anomalies, sd_write median/count,
  sd_read count. Expected if H3b is right: sd_write median drops ≥ 10x,
  sd_read count collapses (multi-sector reads), anomalies and stalls fall.

**Next:** ~16:55 stop RUN-07, analyze, write `runs/RUN-20260828-07.md`
with the A/B table, update README status (Phase 3 baseline done, Phase 4/5
in progress), notify Fab. If stalls persist: Phase 4 provocations (sd
PSRAM-vs-internal, moving-bar) and the H4 question.

## 2026-08-28 — Device renamed: p3a-fab.local; p3a.local is another unit

- Fab named the dev unit "fab" (hostname `p3a-fab`, the 15:33 config-save +
  reboot). `p3a.local` now resolves to a different, unrelated p3a
  (192.168.4.33). All tooling defaults switched to `p3a-fab.local`, and the
  python tools + `soak.ps1` now refuse any host whose `/api/device-name`
  hostname is not `p3a-fab`. RUN-07 was already using the IP (192.168.4.87).

