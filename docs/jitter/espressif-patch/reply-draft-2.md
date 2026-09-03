# Reply draft 2 for esp-idf #19034 (post only with Fab's approval)

Final text (numbers from RUN-20260903-01..03).

---

Tested the new version, same setup and same three-arm procedure as before (v5.5.4 at 73550728, patch applied with `git apply`, our wrap compiled out, arm identity checked on the device).

**Reproducer** (misaligned 32 KB bounce writes under a 30 fps CPU-upscaled GIF): 0 producer anomalies, upscale max 22 ms. Same as v1 and as our variant; stock was 12 / 479 ms.

**Write-completion latency**, median ms per condition, v2 next to yesterday's columns:

| condition (approximate busy period) | stock | v1 | wrap | **v2** |
|---|---|---|---|---|
| internal RAM, 512 B (sub-ms) | 0.7 | 0.9 | 2.5 | 1.1 |
| PSRAM aligned, 512 B (~2 ms) | 2.3 | 4.9 | 2.9 | 2.5 |
| internal RAM, 32 KB (~3 ms) | 2.7 | 4.9 | 3.9 | 3.4 |
| PSRAM aligned, 32 KB (~4.5 ms) | 4.5 | 6.0 | 5.0 | 4.7 |

**Zero-margin probe** (an artwork whose decode fills its frame period, under paced 32 KB writes, the bounce storm and 512 B single-sector bursts): decode flat at 63 ms median / 65 ms max in every condition, 0 anomalies, same as our variant.

**Soak**, 1 h on the normal workload: 48 246 frames, 0 frames late ≥ 100 ms, 0 in 50–100 ms, lateness p99 / max 18.5 / 74.9 ms; 32 KB download writes median / p90 / p99 6.5 / 36.0 / 44.5 ms (v1 was 6.6 / 52.5 / 57.5, our variant 6.2 / 35.6 / 44.3).

From our side this is good to merge. Two things:

- Is a backport to `release/v5.5` planned? We are on v5.5.4 and would like to drop our link-time wrap as soon as a release carries the fix.
- Once it is in a release we will switch p3a to the stock function and report here if anything regresses in the field.

Thanks for turning this around so quickly.

Context, in case it helps: p3a (https://github.com/fabkury/p3a) is an open-source animated pixel art player on the ESP32-P4. It decodes and software-upscales a 720x720 frame every 16 to 60 ms on both cores while background tasks download artwork to the SD card, so a CMD13 storm shows up as a visible freeze of the animation, which is what let us measure this at all. The numbers above come from a frame trace compiled into the firmware (per-frame decode, upscale and presentation lateness, plus timestamped spans around every SD read and write) that a laptop pulls over HTTP during multi-hour soaks, with a provocation endpoint for the controlled writes; three otherwise identical builds differed only in `sdmmc_wait_for_idle()`, and each build reports at runtime which variant it carries. The instrumentation, the host tooling and every run record are in the repo under `docs/jitter/` and `host/jitter-lab/`, if any of it is useful to you.
