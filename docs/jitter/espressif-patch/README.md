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
| Reproducer `wrap` | pending RUN-20260902-04 (after the patch soak) | |
| Soak `patch` 3 h | **running** RUN-20260902-03, started 11:53, "Work mix" (first ~2 min on the bar GIF, see gotcha), `-Hours 3.3` → puller stops ~15:11; Monitor armed in the agent session | `host/jitter-lab/runs/RUN-20260902-03/` |
| Soak `wrap` 3 h | pending | |
| Decision + reply to Espressif | pending | `reply-draft.md` here |
| IDF tree restored, device on release build | pending | |

## Results

(filled in as runs complete; committed summaries in `../runs/`)
