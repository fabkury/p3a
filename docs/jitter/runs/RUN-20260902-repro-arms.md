# RUN-20260902-01 / -02 / -04 — CMD13 poll-storm reproducer, three busy-wait arms (esp-idf #19034)

Same diag build config (release `sdkconfig` + `sdkconfig.diag.defaults`),
same day, same procedure (`arm_reproducer.py`: bar GIF uploaded and playing
under CPU upscale, dwell 3600 s, `sd_experiment.py` 3 rounds), same card
(TOPESEL 32 GB, SDMMC slot 0, 4-bit, 40 MHz, 1000 Hz tick). The arms differ
only in how `sdmmc_wait_for_idle()` waits for the card after a write; the
device confirmed each arm at runtime (`/api/debug/frames/stats` → `config`).

| Arm | Binary | `sdmmc_wait_for_idle()` |
|-----|--------|-------------------------|
| stock (RUN-01) | `build-diag-nowrap` 4f09523b25c4 | IDF v5.5.4 as shipped: CMD13 back-to-back, first yield after 100 ms |
| patch (RUN-02) | `build-diag-patch` 14f03b740e1c | Espressif's patch: 100 µs doubling to 32 ms (sub-tick = spin, else `vTaskDelay`) |
| wrap (RUN-04) | `build-diag` ec3d7abf18ae | p3a fix 8: one CMD13, then one per tick |

## The reproducer condition: 8 x 32 KB `fwrite` from a PSRAM-misaligned buffer (512 B bounce path, one command per sector)

| Arm | writes | write med / max ms | frames in window | upscale med / max ms | producer anomalies (≥ 3x median) |
|-----|--------|--------------------|------------------|----------------------|----------------------------------|
| stock | 51 | 34.0 / 611 | 299 | 16.7 / **479** | **12 (4.0 %)** |
| patch | 51 | 17.3 / 925 | 641 | 16.7 / **22** | **0** |
| wrap | | | | | (pending) |

Baseline outside provocations, both arms so far: 0 anomalies in ~8800 frames,
upscale median 16.4–16.5 ms, max 20 ms.

## Full matrix (all conditions; anomalies were 0 everywhere except stock misaligned 32 KB)

Stock (RUN-01):

| condition | writes | write med/max ms | MB/s | frames in window | upscale med/max ms | anomalies | rate |
|---|---|---|---|---|---|---|---|
| psram-misaligned 512 B | 1563 | 2.4 / 51 | 0.07 | 402 | 16.7 / 53 | 0 | 0.0% |
| psram-misaligned 32768 B | 51 | 34.0 / 611 | 0.07 | 299 | 16.7 / 479 | 12 | 4.0% |
| psram-aligned 512 B | 1571 | 2.3 / 42 | 0.08 | 351 | 16.7 / 28 | 0 | 0.0% |
| psram-aligned 32768 B | 51 | 4.5 / 43 | 1.42 | 39 | 16.6 / 26 | 0 | 0.0% |
| internal 512 B | 1563 | 0.7 / 53 | 0.12 | 239 | 16.7 / 46 | 0 | 0.0% |
| internal 32768 B | 51 | 2.7 / 39 | 1.80 | 35 | 16.6 / 19 | 0 | 0.0% |

Patch (RUN-02):

| condition | writes | write med/max ms | MB/s | frames in window | upscale med/max ms | anomalies | rate |
|---|---|---|---|---|---|---|---|
| psram-misaligned 512 B | 1563 | 5.0 / 59 | 0.04 | 615 | 16.7 / 18 | 0 | 0.0% |
| psram-misaligned 32768 B | 51 | 17.3 / 925 | 0.04 | 641 | 16.7 / 22 | 0 | 0.0% |
| psram-aligned 512 B | 1563 | 4.9 / 64 | 0.06 | 461 | 16.8 / 19 | 0 | 0.0% |
| psram-aligned 32768 B | 51 | 6.0 / 62 | 1.00 | 46 | 16.8 / 19 | 0 | 0.0% |
| internal 512 B | 1563 | 0.9 / 59 | 0.07 | 379 | 16.6 / 18 | 0 | 0.0% |
| internal 32768 B | 51 | 4.9 / 58 | 1.08 | 44 | 16.7 / 18 | 0 | 0.0% |

Wrap (RUN-04): pending.

## Reading so far

- The reproducer still bites on stock (12 anomalies, upscale up to 479 ms:
  the same 3–50x cross-core slowdown as on 2026-08-30) and the patch removes
  it completely (0 anomalies, upscale max 22 ms). On the stall question the
  patch is as good as the wrapper on this hardware.
- The patch's back-off costs write latency. Every single-command write got
  slower: aligned 32 KB (one multi-block command) median 4.5 → 6.0 ms
  (+33 %), 512 B sector writes 2.3–2.4 → 4.9–5.0 ms (about 2x), internal
  32 KB 2.7 → 4.9 ms. That is the overshoot of the doubling sequence
  (0.1, 0.3, 0.7, 1.5, 3.1, 6.3 ms...): a card that is ready at 2 ms is only
  seen at 3.1 ms, one ready at 4.5 ms at 6.3 ms. The wrapper's overshoot is
  bounded by one tick (1 ms here). The wrap arm's numbers will show whether
  the once-per-tick variant keeps the stock write latency.
- Gotcha recorded: `POST /upload` makes a single-artwork playset the active
  one. The soak that followed RUN-02 started on the bar GIF for ~2 min before
  "Work mix" was re-activated by hand; `arm_reproducer.py` now re-activates
  the soak playset itself.
