# Draft: esp-idf issue about the no-yield CMD13 poll in `sdmmc_wait_for_idle()`

Status: DRAFT, not posted. Fab posts it (or not) at https://github.com/espressif/esp-idf/issues.
Written 2026-08-30 from the jitter work stream findings (`REPORT.md` §3.1,
`runs/RUN-20260830-03-04.md`). Fill in the card model before posting.

---

**Title:** sdmmc: `sdmmc_wait_for_idle()` busy-polls CMD13 without yielding for 100 ms after every write, starving both cores on ESP32-P4

### Environment

- ESP-IDF v5.5.4, ESP32-P4 (rev v1.0), Waveshare ESP32-P4-WIFI6-Touch-LCD-4B
- SDMMC host, 4-bit, default clock; FAT filesystem via `esp_vfs_fat_sdmmc_mount`
- Card: `<fill in: brand/model/class>`; card-busy after a single-block write measured at 1 to 45 ms on this card

### Summary

`sdmmc_wait_for_idle()` (`components/sdmmc/sdmmc_common.c`, lines 431 to 459 in v5.5.4) waits for the card to leave the busy state by sending CMD13 back-to-back. It only starts calling `vTaskDelay(1)` after the first 100 ms (`yield_delay_us = 100 * 1000`, doubled each time it fires). Since a card is typically busy for a few milliseconds after a write, in practice the function never yields: every write is followed by a tight loop of hundreds of SEND_STATUS commands until the card reports ready.

On the ESP32-P4 this has a system-wide effect that goes beyond the calling task. While such a poll storm runs on one core, CPU-bound work on **both** cores slows down by 3 to 50 times. In our application (a pixel art player: decode plus software upscale of a 720x720 frame every 16 to 60 ms) this shows up as playback stalls of 100 to 800 ms whenever a background task writes to the card.

### Evidence

Measured with an in-firmware frame trace (per-frame decode and upscale times, plus span marks around every `sdmmc_write_sectors` / `sdmmc_read_sectors` call):

- Normal upscale of one frame: about 14 ms (split across both cores). During a burst of single-block writes from another task on core 0: 50 to 780 ms. Decode on core 1, which does no SD I/O at all, also runs 3 to 4 times slower during the same windows.
- Reproducer: write 8 x 32 KB from a PSRAM buffer whose address or size is not cache-line aligned (so the driver takes its 512-byte bounce path and issues one command per sector). On the stock driver this gives 6 to 9 stalled frames per run with upscale times up to 535 ms.
- Same reproducer with `sdmmc_wait_for_idle()` replaced by a version that polls CMD13 once and then once per FreeRTOS tick: 0 stalled frames, worst upscale 18 ms. SD throughput is unchanged (a 32 KB write still completes in about 5 ms on a healthy card).
- A busy-spin control task at the same priority on the same core does not reproduce the effect, so it is not simple CPU contention. The command storm itself is what slows the other core.

I did not pin down which shared resource is saturated (host controller DMA, the AHB/cache path to PSRAM, or interrupt load from the host ISR). The fix works without that answer.

### Suggested change

Yield between polls from the start, for example:

```c
while (!sdmmc_ready_for_data(status)) {
    if (esp_timer_get_time() - t0 > SDMMC_READY_FOR_DATA_TIMEOUT_US) {
        return ESP_ERR_TIMEOUT;
    }
    if (polls++ > 0) {
        vTaskDelay(1);   // one CMD13 per tick instead of a poll storm
    }
    err = sdmmc_send_cmd_send_status(card, &status);
    ...
}
```

With a 100 Hz tick this adds at most one tick (10 ms) of latency to a write whose busy period is shorter than a tick; at 1000 Hz it is 1 ms. In our measurements the extra latency was not visible in write throughput. If the latency matters for some users, a Kconfig option for the initial yield delay would also solve it.

We currently carry this as a link-time `--wrap=sdmmc_wait_for_idle` in our project (`components/sd_idle_wait/` in https://github.com/fabkury/p3a, `sd_idle_wait.c`). Happy to open a PR if the approach is acceptable.

### Steps to reproduce

1. Run any CPU-bound loop on core 1 and time its iterations (a 1 MB memcpy plus checksum is enough).
2. On core 0, write 8 x 32 KB to the card with `fwrite()` from a plain `heap_caps_malloc(…, MALLOC_CAP_SPIRAM)` buffer (address or size not 128-byte aligned) so the driver takes the 512-byte bounce path and issues one command per sector.
3. Observe the core-1 loop time during the writes versus idle.
4. Repeat with `sdmmc_wait_for_idle()` yielding once per tick from the first poll.
