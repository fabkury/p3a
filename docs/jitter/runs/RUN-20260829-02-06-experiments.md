# 2026-08-29 Phase 4 provocations (RUN-20260829-02..06): the stall trigger is the SD bounce loop

Setup: diag build with fix 1 + fix 2 + provoke variants; `sd_experiment.py`
(SD writes from PSRAM-misaligned / PSRAM-aligned / internal buffers, 512 B vs
32 KB per fwrite, ~256 KB per condition) and `mem_experiment.py` (a task
streaming memcpy+CRC over 64 KB–1 MB of PSRAM on core 0 or 1 at prio 4/6, plus
a busy-spin control).

## Results

- RUN-02-sdexp, RUN-03-memexp (whatever was playing = Giphy/museum = **PPA
  hardware upscale**): 0 anomalies in every condition, even a prio-6 hog on
  core 0. The PPA path does not use the CPU upscale workers; those runs tested
  nothing relevant. Lesson: provocations must run on a CPU-upscaled artwork.
- RUN-04-cpuexp-sd (Makapix "promoted" channel, CPU upscale): **PSRAM-misaligned
  32 KB → 3 anomalies / 63 frames, upscale max 511 ms, one write 587 ms**;
  every other condition 0.
- RUN-06-bar-sd (uploaded `bar_30fps.gif`, 34 fps, CPU upscale, dwell 3600 s):
  **PSRAM-misaligned 32 KB → 6 anomalies / 191 frames (3.1 %), upscale max
  241 ms, write max 408 ms**; misaligned 512 B, aligned 512 B / 32 KB, internal
  512 B → 0. Baseline outside provocations: 0 / 3715.
- Per-task CPU during the reproduced stalls (UART `JTR|T`, 3.8 s windows):
  IDLE0 ≈ 50–160 ms (core 0 saturated), the writing task (httpd) 1.0–1.2 s,
  upscale_top 1.8–2.1 s and upscale_bottom 1.7–2.0 s (about 2x their normal),
  producer 0.6–0.7 s.

## Mechanism

`sdmmc_write_sectors()` with a PSRAM buffer that is not 128-byte aligned
bounces 512 B at a time: 64 single-block write commands per 32 KB, each
followed by `sdmmc_wait_for_idle()`, which polls CMD13 in a tight loop with no
yield for the first 100 ms. A single-block write keeps this card busy 1–35 ms,
so the writer hammers the host controller (command + ISR + wake) for most of
that time, on core 0, 64 times per chunk. That churn eats core 0 (the
`upscale_top` worker lives there) and the producer waits for both halves, so a
frame's upscale goes from 16 ms to hundreds. A 512-byte fwrite through FATFS
is a single command with FATFS work between commands and does not trigger it;
an aligned buffer is one multi-block command and does not trigger it.

So: fix 1 (`http_fetch`, `loader_service`) removed the download/artwork-load
bounce; the residual stalls were `event_bus` channel-cache saves from
`psram_malloc` buffers, aligned only by luck; fix 2 aligns those. Nothing
about SD DMA on the bus, PSRAM bandwidth or L2 pressure reproduced (H4 closed).

## Incident during the experiments (my error)

`snapshot_settings.py restore` sent a partial `PUT /config` WITHOUT
`?merge=true`; that endpoint replaces the whole config, so the device lost its
API keys, `sdcard_root=/p3a2` and `device_name=fab` (hostname fell back to
`p3a`; the identity guard caught it when the next soak refused to start).
Restored from the day-1 capture (all fields verified), device rebooted, name
and keys confirmed. Tool hardened: merge=true always, and restore only touches
fields the tool itself changed (`settings_changed.json`).
