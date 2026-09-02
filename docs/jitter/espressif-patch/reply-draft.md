# Reply draft for esp-idf #19034 (post only with Fab's approval)

Placeholders in [brackets] are filled from RUN-20260902-06 once it ends.

---

Tested. The patch fixes the stall. Numbers below, same hardware and settings as the report (ESP32-P4 rev v1.0, SDMMC slot 0, 4-bit, 40 MHz, TOPESEL 32 GB card, 1000 Hz tick), IDF v5.5.4 at 73550728 with the patch applied with `git apply`. Our link-time wrap was compiled out for the patched build and the device reports at runtime which variant it runs, so the arms are what they claim to be.

Three builds that differ only in `sdmmc_wait_for_idle()`:

- **stock**: v5.5.4 as shipped
- **patch**: your patch, default `CONFIG_SD_READY_POLL_PERIOD_START_US=100`
- **wrap**: our once-per-tick variant

**1. Reproducer** (8 x 32 KB `fwrite` from a PSRAM buffer that is not cache-line aligned, so the driver takes the 512 B bounce path and issues 64 single-block writes per chunk, while a 30 fps GIF plays under CPU upscale; 3 rounds each):

| arm | frames in the write windows | upscale median / max ms | frames with producer time ≥ 3x median |
|---|---|---|---|
| stock | 299 | 16.7 / **479** | **12** (4.0 %) |
| patch | 641 | 16.7 / 22 | 0 |
| wrap | 323 | 16.8 / 18 | 0 |

Every other condition of the matrix (aligned PSRAM, internal RAM, 512 B and 32 KB) was 0 on all three arms. So on this board the patch removes the cross-core slowdown completely, same as our variant.

**2. Soak**, normal workload (image player rotating over network downloads to the card, 3.3 h per arm, same day, same playlist):

| arm | frames | presented-frame lateness ≥ 100 ms | 50–100 ms | lateness p99 / max ms |
|---|---|---|---|---|
| patch | 160 760 | 0 | 1 | 39.1 / 77.2 |
| wrap | [N] | [S] | [W] | [p99] / [max] |

No regression either way. (The patched run had four reboots from a loose USB cable on our side, not the firmware; they are classified in our logs.)

**3. Write-completion latency.** This is where the two approaches differ. Median duration of `sdmmc_write_sectors()` including the wait, per condition of the reproducer matrix:

| condition (approximate busy period) | stock | patch | wrap |
|---|---|---|---|
| internal RAM, 512 B (sub-ms) | 0.7 | 0.9 | 2.5 |
| PSRAM aligned, 512 B (~2 ms) | 2.3 | 4.9 | 2.9 |
| internal RAM, 32 KB (~3 ms) | 2.7 | 4.9 | 3.9 |
| PSRAM aligned, 32 KB (~4.5 ms) | 4.5 | 6.0 | 5.0 |

And over the soaks, the duration of the application's ordinary 32 KB download writes (this card is busy 20 to 45 ms after those):

| arm | SD write median / p90 / p99 ms |
|---|---|
| patch | 6.6 / 52.5 / 57.5 |
| wrap | [med] / [p90] / [p99] |

The doubling sequence lands at 0.1, 0.3, 0.7, 1.5, 3.1, 6.3, 12.7, 25.5, 57.5 ms, so a busy period of 4.5 ms is seen at 6.3 ms and one of 30 to 45 ms at 57.5 ms. The per-tick variant is within one tick of the card on those, but loses on sub-millisecond busy periods, where the spins in your patch are the better tool.

**Suggestion.** Keep the patch's shape (spin for the sub-tick part, then `vTaskDelay`) but cap the back-off at one tick period instead of 32 ms: once the delay is a tick, `vTaskDelay(1)` already yields, and one CMD13 per tick is not a storm (we measured no cross-core effect from paced commands at that rate). Growing past a tick only adds overshoot. With that cap the 100 Hz users get the same behaviour they get now up to 10 ms, and 1000 Hz users get the write latency of the table's `wrap` column without its sub-millisecond loss. If you prefer to keep the 32 ms cap for the erase-style long waits, a Kconfig for the cap next to the one for the start period would let us set it.

Either way the patch is good to merge from our side. The backport to `release/v5.5` question from my previous comment still stands: we will drop our wrap as soon as a release carries the fix.

[Optional closing line, Fab's call: thanks for the fast turnaround.]
