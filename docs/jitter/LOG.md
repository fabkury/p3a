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
