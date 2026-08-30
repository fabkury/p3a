# Jitter Work Stream — Final Report (draft, Phase 7 in progress)

Companion to `README.md` (status, rules, environment), `PLAN.md` (hypotheses,
design, decision records) and `LOG.md` (journal). This file is the standalone
summary: what the stalls were, what caused them, what was changed, and the
evidence. Written 2026-08-30; the Phase 7 section is filled in when
RUN-20260830-06 concludes.

## 1. Problem and pass bar

p3a occasionally froze an animation for ~100–500 ms and then resumed, on any
artwork, sporadically, under the normal workload (Makapix + Giphy/Klipy +
museum channels, Wi-Fi up). Out of scope by decision: uniform slowdown on
artworks the chip cannot decode in time (producer overrun), and 60 Hz
quantization judder.

Pass bar (Fab, 2026-08-28): **no presented-frame lateness ≥ 100 ms during a
multi-hour soak on the normal workload.** Warning tier 50 ms.

## 2. Result in one table

Same playset ("Work mix"), same device (dev unit `p3a-fab.local`, COM5),
in-scope stalls per hour as reported by `host/jitter-lab/analyze.py`
(overrun frames excluded):

| Build | Fixes | Run | Hours | Stalls ≥100 ms | /h | Warns | p99 lateness |
|---|---|---|---|---|---|---|---|
| unfixed | — | RUN-20260828-06 | 0.85 | 32 | 37.7 | 22 | 145 ms* |
| fix 1 | aligned SD DMA buffers | RUN-20260829-01 | 0.64 | 5 | 7.8 | 4 | |
| fixes 1–2 | + aligned psram_alloc | RUN-20260829-07 | 1.84 | 5 | 2.7 | 2 | |
| fixes 1–3 | + band-stealing upscale | RUN-20260829-10 | 0.96 | 3 | 3.1 | 1 | |
| fixes 1–5 | + paced cache flush, verify batches | RUN-20260829-11 | 4.63 | 2 | 0.43 | 1 | |
| fixes 1–6 | + POSIX read() artwork loads | RUN-20260829-12 | 12.07 | 7 (all `ota_check`) | 0.58 | | |
| fixes 1–7b + Phase 6 | + networking on core 0, re-baseline | RUN-20260830-03 | 3.75 | 1 | 0.27 | 1 | 41 ms |
| fixes 1–8 + Phase 6 | + yielding SD idle wait | RUN-20260830-05 | 2.70 | **0** | **0** | **0** | 37.6 ms |
| Phase 7: release config + trace | same code, no diag scheduler | RUN-20260830-06 | (running) | | | | |

\* p99 in the unfixed run is dominated by the overrun sawtooth that Phase 6
later removed; the stall column is the comparable number.

Every run summary lives in `runs/RUN-*.md` with the raw numbers, the
attribution tables and the reasoning that led to the next fix.

## 3. What the stalls were

The frame trace (`components/frame_trace`, Kconfig `P3A_FRAME_TRACE`, default
off) records, per presented frame, when it was due, when it hit the panel, and
how long its decode and upscale took, plus span marks for SD reads/writes,
flash ops, NVS commits, downloads, cache refreshes, HTTP requests and MQTT
receives. The analyzer separates **producer-bound** frames (the frame was not
ready in time because the artwork is too heavy: out of scope) from **in-scope
stalls** (the frame was late although the pipeline should have kept up).

Two independent mechanisms were found:

### 3.1 SD command storms slow both cores (the dominant family)

Signature: a single frame whose **upscale** time balloons from ~14 ms to
50–780 ms while decode is unchanged, or a decode that runs 3–4× slower for a
few hundred ms, always overlapping bursts of SD activity by another task
(`download_mgr`, `event_bus`, `anim_loader`).

Root physics (H3c, `runs/RUN-20260830-03-04.md`): after every SD write, IDF's
`sdmmc_wait_for_idle()` polls CMD13 (SEND_STATUS) **without yielding** for up
to 100 ms until the card reports ready. This card is busy 1–45 ms per block, so
each write is followed by hundreds of back-to-back SD commands. During such a
storm, decode and upscale on **both** cores run 3–50× slower (measured; the
exact hardware reason, presumably the SD host's DMA/AHB traffic and interrupt
load, was not pinned down further). Substituting a yielding wait (one CMD13 per
FreeRTOS tick) turns the on-demand reproducer from 6–9 anomalies / 535 ms into
0 / 18 ms.

Everything that multiplied the number of SD commands multiplied the exposure:

- **Bounce path (H3b).** The ESP32-P4 SD host DMAs directly to PSRAM only when
  the buffer address and size are both 128-byte (cache-line) aligned; otherwise
  it bounces 512 B per command. `http_fetch`'s 32 KB download chunk and the
  loader's file buffer both failed the check: 64 commands per 32 KB write,
  ~1000 per 500 KB artwork read.
- **stdio sector reads.** newlib's stdio buffer is 512 B
  (`CONFIG_FATFS_VFS_FSTAT_BLKSIZE=0`), so `fread()` fetched one sector per SD
  command regardless of the request size: a 4.6 MB artwork was 9523 reads
  over 5.6 s. `setvbuf(_IONBF)` did not help (newlib then reads through a
  1-byte buffer).
- **Back-to-back cache saves** (`channel_cache_flush_all`: temp file + rename +
  FAT/directory updates per cache) and the **LAI verify sweep** (batches of 16
  `stat()` = 170–209 directory reads per ~100 ms).

### 3.2 Priority inheritance drags a network task onto the render core

Signature (RUN-20260829-12, the overnight run): lateness 600–700 ms with the
frame ready 164 ms **early**, i.e. the consumer itself was preempted. The
periodic OTA check (`ota_check`, prio 3, unpinned) inherits priority 18 from
lwIP's `tcpip` task while holding a socket/TLS lock and, unpinned, lands on
core 1 and runs above the consumer (prio 6). Reproduced 3/3 with
`POST /ota/check`; 0/3 after the fix.

### 3.3 Not the cause (tested and closed)

- PSRAM bandwidth saturation (H4): memcpy/CRC streams over 64 KB–1 MB on
  either core at prio 4/6, busy-spin controls, cache-sync provocations: no
  stalls.
- Flash/NVS writes (H1): forced NVS commits produced nothing measurable.
- Logging (H5): a log flood adds ~18 % upscale drag but no stalls.
- Interrupts-off windows on core 0 (H6) do not cause lateness but drop panel
  frames (one-frame blue flash); found in my own diagnostics
  (`uxTaskGetSystemState` once per second) and removed. Release code was not
  observed doing it.

## 4. What changed

All fixes are on `main` (`8cf28935`); each is its own commit with the
before/after runs linked in `LOG.md`.

| # | Commit (main) | Change |
|---|---|---|
| 1 | `6ca1e7c5` | `http_fetch` chunk and `loader_service` file buffer 128-byte aligned (address and size): SD DMA goes straight to PSRAM |
| 2 | `a2650d2e` | `psram_malloc/calloc` 128-byte aligned (`PSRAM_ALLOC_ALIGN`), so every PSRAM buffer that reaches FATFS qualifies |
| 3 | `69ba412f` | CPU upscale: work-stealing 24-row bands from one atomic counter instead of a fixed top/bottom split (a stalled core costs one band, not half the frame) |
| 4 | `30805a68` | `channel_cache_flush_all`: 300 ms gap between dirty-cache saves |
| 5 | `4f389697` | LAI verify sweep: batches of 4 `stat()` with 40 ms pauses |
| 6 | `944dd73a` | Artwork loads and pinned-artwork copies via POSIX `read()` (whole-cluster transfers); channel cache loads get a file-sized aligned stdio buffer |
| 7 | `a61c3c69` | lwIP `tcpip` pinned to core 0 (`CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0=y`, the only functional release `sdkconfig` change; the other diff is the `# CONFIG_P3A_FRAME_TRACE is not set` menu lines) and `ota_check` pinned to core 0 |
| 7b | `24164cb3` | Every networking/SD/event task and the HTTP server pinned to core 0 (32 call sites) |
| 8 | `8cf28935` | New always-on component `sd_idle_wait`: link-time `--wrap` of `sdmmc_wait_for_idle` that yields one tick between CMD13 polls. Removes the root physics; fixes 1–6 remain as command-count reductions |

On the branch (`feat/jitter`), to land with the final merge:

- **Phase 6 catch-up policy** (`main/display_renderer.c`): a frame that is
  late because the producer is late re-baselines the playhead to now (uniform
  slowdown) instead of accumulating toward the 250 ms resync and snapping;
  frames ready in time but presented late keep the catch-up path. On
  producer-bound artworks: 0 resyncs, p99 lateness 245 → 41 ms, no periodic
  skip.
- **Diagnostics**, all behind `CONFIG_P3A_FRAME_TRACE` (default off, zero
  code/data in release): frame ring + span marks, stall detector with
  per-generation EMA gating, UART `JTR|` reporter, `/api/debug/frames*`
  endpoints, FPS-overlay worst-lateness readout, dev-only provocation
  endpoints (`P3A_FRAME_TRACE_DEV_ENDPOINTS`), `sdkconfig.diag.defaults` and
  `sdkconfig.trace.defaults` overlays.
- **Host lab** `host/jitter-lab/`: reset-free UART logger, HTTP frame puller
  (reboot-aware), analyzer, run comparator, soak orchestrator, settings
  snapshot/restore (keys redacted), experiment and stress scripts.

## 5. Verification

- **A/B pairs** for every fix (same playset, same probes), see §2.
- **On-demand reproducers**: `sd_experiment.py` (bounce vs aligned vs internal
  buffers, 512 B vs 32 KB writes), `provoke_experiment.py` (cache sync,
  memory streams, spins, log floods), `POST /ota/check` ×3.
- **Stress** (`stress.py`, RUN-20260830-02): rapid swaps, playset switches,
  a web-UI polling storm, OTA checks, forced refreshes, uploads: 0 stalls in
  all six phases; uploads show sub-100 ms hiccups (fix 9 candidate below).
- **Overnight**: 12 h clean apart from the OTA class that became fix 7.
- **Phase 7** (release `sdkconfig` + `sdkconfig.trace.defaults`, no FreeRTOS
  run-time stats, no dev endpoints): RUN-20260830-06, result pending.

## 6. Residuals and follow-ups

- **Fix 9 candidate**: `/upload` writes in chunks that are not sector
  multiples; sub-100 ms hiccups during uploads (never a stall in the soaks).
- **Upstream**: report the no-yield CMD13 poll in `sdmmc_wait_for_idle()` to
  Espressif (esp-idf issue), with the reproducer numbers.
- **Unexplained pair** in RUN-20260829-10 (#3/#4: 257/234 ms with no SD
  activity; consumer vsync wait 143 ms while the UART reporter printed). Not
  seen again after fix 7/7b.
- The exact hardware reason a CMD13 storm slows both cores stays open; the
  fix does not depend on it.
- Phase 4 leftover: a `cpu0_critical` provoke (H6) was never built; H6 is
  glass-only and Fab's eyes are the detector.

## 7. How to re-run

`README.md` §Resume protocol. Short form: `host/jitter-lab/build.ps1 [-Diag]
[-Flash]`, `soak.ps1 -Start -Run <id> -DeviceHost http://p3a-fab.local`,
`analyze.py <id>`, `compare_runs.py A B`. Tooling refuses any device whose
hostname is not `p3a-fab`.
