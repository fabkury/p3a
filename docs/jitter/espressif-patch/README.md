# Espressif patch evaluation (esp-idf #19034) — START HERE

Sub-stream of the jitter work stream (`../README.md`). Espressif answered our
issue https://github.com/espressif/esp-idf/issues/19034 on 2026-09-02 with a
patch for v5.5.4 and asked us to test it. This folder is the resumable record
of that test: what we compare, how, where the data is, and what was decided.

If you are a fresh session: read this file, then the **Status** table, then
the last entry of `../LOG.md`. The device may be mid-soak; check with
`soak.ps1 -Status` before touching it.

## The question

Espressif's patch (`sdmmc_back_off_between_CMD13_polls_v5.5.4.patch`, saved
as `patch/` here) replaces the 100 ms no-yield CMD13 poll storm with an
exponential back-off: 100 µs doubling to a 32 ms cap; delays shorter than one
FreeRTOS tick are `esp_rom_delay_us()` spins, longer ones `vTaskDelay()`. It
patches all three busy loops (`sdmmc_wait_for_idle`, init data-ready wait,
tuning-block read) and adds `CONFIG_SD_READY_POLL_PERIOD_START_US`.

Our fix 8 (`components/sd_idle_wait`) polls once, then once per tick.

Decide: (1) does the patch remove the cross-core stall on our hardware as
well as our wrapper does; (2) which one p3a ships; (3) what we tell Espressif.

## Arms

Three binaries, all **diag** flavour (release `sdkconfig` +
`sdkconfig.diag.defaults`: frame trace, provoke endpoints, FreeRTOS run-time
stats), differing only in the busy-wait:

| Arm | Build dir | IDF tree | `CONFIG_P3A_SD_IDLE_WAIT_WRAP` | Device reports (`/api/debug/frames/stats` → `config`) |
|-----|-----------|----------|-------------------------------|------------------------------------------------------|
| `stock` | `build-diag-nowrap/` | clean 73550728 | n (`sdkconfig.nowrap.defaults`) | `sd_idle_wait_wrap=false, idf_sdmmc_backoff_patch=false` |
| `patch` | `build-diag-patch/` | patch applied | n | `sd_idle_wait_wrap=false, idf_sdmmc_backoff_patch=true` |
| `wrap` (ours, = main) | `build-diag/` | clean | y (default) | `sd_idle_wait_wrap=true, idf_sdmmc_backoff_patch=false` |

Arm identity is proven twice: `build.ps1` prints an `ARM:` line from the map
file (`__wrap_sdmmc_wait_for_idle`, `sdmmc_poll_delay_and_backoff`) and the
device reports the same two facts at runtime (`sd_idle_wait_info.c`, weak
reference to the patch's new symbol). `arm_reproducer.py --arm X` refuses to
run if the device does not match.

## Procedure

1. **Reproducer, all three arms** (minutes each): `arm_reproducer.py RUN --arm X`
   uploads the 30 fps bar GIF (CPU upscale, constant decode cost), dwell
   3600 s, then `sd_experiment.py` (3 rounds of the SD write matrix; the
   PSRAM-misaligned 32 KB condition is the poll-storm reproducer that gave
   6–9 anomalies / upscale max 535 ms on stock and 0 / 18 ms with the wrapper
   on 2026-08-30). Settings restored afterwards.
2. **Soak, `patch` and `wrap`** (3 h each, "Work mix" playset, same day):
   `soak.ps1 -Start -Run RUN -Hours 3.3 -Every 60`, then `-Stop`,
   `analyze.py`, `run_audit.py` (reboot classification + coverage gaps),
   `compare_runs.py`. Pass bar as before: no in-scope presented-frame lateness
   ≥ 100 ms.
3. Decision, `../LOG.md` entry, `runs/RUN-*.md` summaries, reply on the issue
   (drafted here, posted only with Fab's approval), device back on a release
   build from `main` with the IDF tree restored.

### Order of operations (minimises flashes and keeps the IDF tree honest)

```
build wrap + nowrap while the IDF tree is clean          (done first, binaries persist)
git -C C:/esp/v5.5.4/esp-idf apply <patch>; build patch  (IDF tree now dirty)
flash stock (-FlashOnly) -> reproducer
flash patch (-FlashOnly) -> reproducer -> soak 3 h
flash wrap  (-FlashOnly) -> reproducer -> soak 3 h
git -C C:/esp/v5.5.4/esp-idf checkout -- components/sdmmc && rm components/sdmmc/Kconfig
release build (build.ps1, guard: release sdkconfig unchanged) -> flash -> verify /api/debug 404
```

`-FlashOnly` flashes with esptool from the build dir's `flash_args` and never
re-runs ninja, so a binary built against one IDF tree state cannot be silently
rebuilt against another. A plain `-Flash` or `idf.py flash` WOULD rebuild.

## Robustness rules

- **Accidental resets are expected** (USB-C cable). The puller opens a new
  `frames.e<N>.csv` epoch on each reboot; `run_audit.py` classifies every
  UART `rst:` line (POWERON/brownout = external; panic/watchdog = firmware;
  `Restarting` = deliberate). A run is only invalidated by a firmware-class
  reset; an external reset costs the ring contents of the seconds before it.
- **Laptop sleep is a coverage gap, not a fault.** Logger and puller are
  detached and reconnect; the device ring holds ~15 min at 30 fps, so a longer
  sleep loses frames but the device keeps running. `run_audit.py` lists gaps
  from puller heartbeats; covered hours are what the summary reports.
- **Never flash while the soak's logger holds COM5**: `soak.ps1 -Stop` first.
- Release `sdkconfig` must not gain `CONFIG_SD_READY_POLL_PERIOD_START_US`:
  never run a release build while the IDF tree is patched (`build.ps1` guard
  throws on any release sdkconfig change).

## Status

| Step | Status | Where |
|------|--------|-------|
| Kconfig switch `P3A_SD_IDLE_WAIT_WRAP` + runtime arm identity + `build.ps1 -Extra/-Suffix/-FlashOnly` + `run_audit.py` + `arm_reproducer.py` | done 2026-09-02 | this commit |
| Build `wrap` + `nowrap` (clean IDF) | done 11:32 (sha ec3d7abf18ae / 4f09523b25c4) | `build-diag/`, `build-diag-nowrap/` |
| Apply patch to IDF, build `patch` | done 11:36 (sha 14f03b740e1c); **IDF tree is dirty until restored** | `build-diag-patch/` |
| Reproducer `stock` | done RUN-20260902-01: misaligned 32 KB → 12 anomalies / 299 frames (4.0 %), upscale max 479 ms; all other conditions 0 | `../runs/RUN-20260902-repro-arms.md` |
| Reproducer `patch` | done RUN-20260902-02: 0 anomalies in every condition, upscale max 22 ms; but single-command writes ~1.5–2x slower (aligned 32 KB median 4.5 → 6.0 ms) | same file |
| Reproducer `wrap` | done RUN-20260902-05: 0 anomalies everywhere, upscale max 18 ms; write latency within ~1 ms of stock except sub-ms busy periods (tick granularity) | same file |
| Soak `patch` 3 h | **done RUN-20260902-03: PASS**, 3.30 h, 160 760 frames, 0 stalls, 1 warn, p99 39.1 ms, max 77.2 ms; 4 reboots all external (cable, confirmed by Fab); SD write p90/p99 52.5/57.5 ms vs 37.2/46.8 on the wrapper (RUN-06) | `../runs/RUN-20260902-03.md` |
| Upload stress `patch` | done RUN-20260902-04: 2342 frames, 0 anomalies, 0 stalls, upscale max 20 ms (wrapper reference RUN-20260830-08: 2 anomalies, max 21 ms) | `host/jitter-lab/runs/RUN-20260902-04/` |
| Soak `wrap` 3.4 h | done RUN-20260902-06 (1.80 h, **2 stalls** = one event on a zero-margin artwork during download writes + loader reads) + RUN-20260902-07 continuation (1.63 h, 0 stalls); p99 42.0 / 34.3 ms; SD write p90/p99 35.6/44.3 ms | `../runs/RUN-20260902-06-07.md` |
| Upload stress `wrap` | done RUN-20260902-08: 2325 frames, 0 anomalies, 0 stalls, upscale max 21 ms | |
| Zero-margin probe `wrap` | done RUN-20260902-09/09L: decode flat at 64 ms under every write condition incl. the storm control; the RUN-06 class needs loader reads and is arm-independent | same file |
| Decision + reply to Espressif | done: reply posted 2026-09-02 19:54 local as https://github.com/espressif/esp-idf/issues/19034#issuecomment-5518162367 (text in `reply-draft.md`) | |
| IDF tree restored, device on release build | done 19:40: tree clean, `build/` acbc70f6c424 flashed, `/api/debug/*` 404, Work mix playing | |

## Resume protocol (historical)

Used once, 2026-09-02 17:15 → 17:47 (laptop sleep between two locations).
Kept short: the on-disk state was (device on the wrap arm, IDF tree patched,
no soak processes, all runs committed) and the steps were the remaining rows
of the Status table. If this evaluation is ever re-run, start from the
Status table and `../LOG.md`.

## Results

| | stock | patch | wrap (ours) |
|---|---|---|---|
| Reproducer (misaligned 32 KB bounce storm): producer anomalies / upscale max | 12 / 479 ms | 0 / 22 ms | 0 / 18 ms |
| Soak, Work mix, same day: stalls ≥ 100 ms / hours | (not run) | 0 / 3.30 h | 2 / 3.43 h (one event, see below) |
| Soak lateness p99 / max | | 39.1 / 77.2 ms | 42.0 / 231.9 ms (RUN-06), 34.3 / 75.4 ms (RUN-07) |
| Upload stress: anomalies / stalls | | 0 / 0 | 0 / 0 |
| SD write spans over the soak: p90 / p99 | | 52.5 / 57.5 ms | 35.6 / 44.3 ms |
| Write latency, ms-scale busy periods (reproducer medians) | floor | up to 2x stock | within ~1 ms of stock |
| Write latency, sub-ms busy periods | 0.7 ms | 0.9 ms | 2.5 ms |

The wrap soak's one stall event (RUN-06, post 3621) is a producer-bound
artwork meeting download writes plus loader reads queued behind them. The
zero-margin probe shows the wrapper's one-poll-per-tick traffic does not
inflate a saturated decoder at all, and loader reads never enter
`sdmmc_wait_for_idle()`, so the event is not a property of either busy-wait.
The patch soak simply never drew that artwork (0 picks vs 8). It stays on
the books as the residual class from RUN-11 (2026-08-29), to be probed
separately.

## Round 2 (2026-09-03): Espressif's revised patch (one-tick cap)

Adam adopted the suggestion the same night: `patch/newer_sdmmc_back_off_between_CMD13_polls_v5.5.4.patch`
(v2) doubles from 100 µs, spins while below one FreeRTOS tick, then one
`vTaskDelay(1)` per poll; both the delay and the configured start period
are clamped to a tick (Kconfig range 1–10000 µs, `SDMMC_READY_POLL_PERIOD_MAX_US`
gone). At 1000 Hz this is our wrapper plus 100/200/400/800 µs spins before
the first yield. He asked whether it is OK to merge to master. Fab's call:
test it on the device first (reproducer, zero-margin probe, 1 h soak), then
answer with numbers; re-ask about the `release/v5.5` backport; offer to close
the loop in p3a.

| Step | Status | Where |
|------|--------|-------|
| Apply v2 to the IDF tree, build `patch2` (`build-diag-patch2/`, wrap off) | done 11:57 (sha e8690dc8bec7), flashed 12:00; **IDF tree dirty until restored** | |
| Reproducer `patch2` | done RUN-20260903-01: 0 anomalies, upscale max 22 ms; write medians internal 512 B 1.1, aligned 512 B 2.5, internal 32 KB 3.4, aligned 32 KB 4.7 ms (stock 0.7 / 2.3 / 2.7 / 4.5; v1 0.9 / 4.9 / 4.9 / 6.0; wrap 2.5 / 2.9 / 3.9 / 5.0) | `../runs/RUN-20260903-v2.md` |
| Zero-margin probe `patch2` | done RUN-20260903-02: decode 62.8–63.0 / max 63–65 ms in every condition, 0 anomalies | same file |
| Soak `patch2` 1 h | done RUN-20260903-03: 1.08 h, 48 246 frames, 0 stalls, 0 warns, p99 18.5 ms, max 74.9 ms; SD write p90/p99 36.0/44.5 ms | same file |
| Reply (tested OK) | text final in `reply-draft-2.md`, awaiting Fab's approval | |
| IDF tree restored, device on release | done 13:25: tree clean, `build/` acbc70f6c424 flashed, `/api/debug/*` 404 | |

## Decision (2026-09-02)

**p3a keeps fix 8 (the once-per-tick wrapper) until an ESP-IDF release
carries Espressif's fix; then the wrap comes out and the Kconfig stays.**

- Both approaches kill the poll storm completely on this hardware; the
  soaks are equivalent on stalls.
- The wrapper has the better write-completion tail for our workload (32 KB
  download writes on a card that is busy 20–45 ms): p99 44 vs 57 ms. The
  patch wins only on sub-millisecond busy periods, which p3a does not care
  about.
- The wrapper is what v1.2.1 shipped and has field time; a patched IDF tree
  is not something a public project can ask its builders to carry.
- To Espressif: the patch is good to merge; the suggested refinement is to
  cap the back-off at one tick period (the spin part of the patch fixes the
  wrapper's only weakness, the tick-size overshoot on sub-ms busy periods),
  and a backport to `release/v5.5`.

