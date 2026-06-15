# SDIO RX buffer OOM crash (esp_hosted streaming mode)

**Status:** Open. Heap-snapshot diagnostic in firmware and producing
data; fragmentation+exhaustion hypothesis confirmed. Trigger pattern
now confirmed to extend to **museum-channel HTTPS paging**
(Occurrence 6) and to **Makapix MQTT channel-index refresh at high
channel count** (Occurrence 7), not just Giphy. The A/B decision is
strongly indicated but not yet implemented; **Option E partially landed
2026-06-02** (cJSON parse heap routed to PSRAM — see "Option E" under
"Decision required"). Occurrence 7 adds a **new victim (SD path) and a
new, non-crashing failure mode (persistent livelock)** that A alone does
not fix — see "Occurrence 7" and "New failure mode" below.
Upstream sibling assert site fixed 2026-04-29 (not our site); see
"Upstream developments" below.
**First observed:** 2026-05-04.
**Last observed:** 2026-06-14 (seventh occurrence; a deliberate 64-channel
Makapix stress test drove the DMA-internal pool to `free=1863` — lower than
ever recorded — and the SD path, not the SDIO RX assert, was the victim:
**no crash, but a persistent SD-I/O livelock until manual reboot**).
**Severity:** Hard crash + reboot for Occurrences 1–3, 5–6; one near-miss
(Occurrence 4); **one persistent livelock with no reboot (Occurrence 7)**.
**Confirmed recurring across builds and IDF versions** — five hard crashes,
one near-miss, and one livelock spanning 2026-05-04 → 2026-06-14, on at
least four firmware builds (ELF `969b00418` for Occurrences 2–4, ELF
`fc8daa29f` for Occurrence 5, ELF `ee443f31c` for Occurrence 6, firmware
`1.0.0` for Occurrence 7) and across ESP-IDF v5.5.1 and v5.5.2.

---

## TL;DR

The ESP32-P4 panics with an unconditional assert inside the esp_hosted SDIO
driver when it can't allocate a growable RX buffer for traffic from the
ESP32-C6 Wi-Fi co-processor. The assert fires under simultaneous Giphy refresh
+ active GIF downloads + animation playback. The cleanest fix is a Kconfig
change to switch the SDIO RX mode from streaming to `MAX_SIZE`
(preallocated buffer), trading some Wi-Fi throughput for stability.

**New as of Occurrence 7 (2026-06-14):** the same DMA-internal pool
exhaustion can surface *without a crash*. When the **SD card / SDMMC path**
is the victim instead of esp_hosted's RX buffer, the failing allocation
returns `ESP_ERR_NO_MEM` (SDMMC degrades gracefully) rather than asserting —
so the device does **not** reboot. With nothing to clear the pool and no
load-shedding to let it drain, the firmware wedges into a **persistent
SD-I/O-failure livelock** until manually rebooted. This is strictly worse
than the crash: a reboot self-heals; the livelock does not. It also proves
the proposed fix **A (packet mode) is necessary but not sufficient** — A
removes the *panic* class but not the SD-starvation livelock.

---

## Crash details

```
assert failed: sdio_rx_get_buffer sdio_drv.c:830 (*buf)    # Occurrences 1–4 (IDF v5.5.1, esp_hosted older)
assert failed: sdio_rx_get_buffer sdio_drv.c:896 (*buf)    # Occurrences 5–6 (IDF v5.5.2, esp_hosted newer)
Core 1 register dump:
MEPC    : 0x4ff0a7ce  (panic_abort)
MCAUSE  : 0x00000002  (illegal instruction — the unimp from panic_abort)
```

Failing line in
`managed_components/espressif__esp_hosted/host/drivers/transport/sdio/sdio_drv.c`
(line :830 on IDF v5.5.1, :896 on IDF v5.5.2 — same code, esp_hosted shifted
between versions):

```c
*buf = (uint8_t *)g_h.funcs->_h_malloc_align(len, HOSTED_MEM_ALIGNMENT_64);
assert(*buf);   // <-- fired: returned NULL
```

This is the streaming-mode RX buffer growth path. When an incoming SDIO chunk
is larger than the current buffer slot, the driver frees and reallocates the
slot with `_h_malloc_align()`. If that allocation fails it panics —
no fallback, no graceful drop.

---

## What was happening at the moment of crash

### Occurrence 1 — 2026-05-04 (uptime ~2608 s)

Timeline of the last ~20 seconds before panic (timestamps in ms since boot):

| t (ms)  | Event |
|---------|-------|
| 2588066 | `ps_refresh` starts periodic refresh cycle |
| 2588190 | Refresh **Giphy Trending**: 3 paginated HTTPS fetches (97 KB + 97 KB + 76 KB) |
| 2593799 | Trending refresh complete; LAi rebuilt (115 files) |
| 2595678 | `giphy_dl` downloads `AyhyKY14ZXfxe` (291 KB) — TLS handshake |
| 2601235 | `giphy_dl` downloads `SdcUyXudSnRS327UMt` (438 KB) — TLS handshake |
| 2603265 | `giphy_dl` downloads `OsfVaOer7N2265YTRF` (112 KB) — TLS handshake |
| 2605962 | Next periodic refresh fires (10s after previous, by design) |
| 2606097 | Refresh **Giphy Work** (search `q="work"`) — page 1 fetched (96 KB) |
| 2607885 | Page 2 fetch starts (`offset=50`) |
| 2608022 | TLS cert validated for page 2 |
| —       | **CRASH** (no further log line) |

`ps_pick` was running throughout (stochastic channel selection + frame load),
so the animation player was actively decoding frames in parallel with all of
the above.

### Why two refreshes back-to-back?

After Trending finished, the scheduler logged
`Next periodic refresh in 10 seconds`. The other channels were already 3545s,
3572s, and 3582s into their 3600s freshness window — so 10s later their
windows expired and the scheduler went back to work. This is by design but it
means we get bursts of network activity rather than evenly spaced refreshes.

### Occurrence 2 — 2026-05-05 (uptime ~1404 s)

ELF SHA256: `969b00418…`. Crash text identical (`assert failed:
sdio_rx_get_buffer sdio_drv.c:830 (*buf)`, MEPC `0x4ff0a7ce`).

Timeline of the last ~10 seconds before panic (timestamps in ms since boot):

| t (ms)  | Event |
|---------|-------|
| 1345509 | `ps_refresh` cycle starts; only **Giphy Trending** needs refresh |
| 1345639 | Trending refresh: 5 paginated HTTPS fetches (97 + 98 + 98 + 98 + 72 KB) |
| 1355120 | Trending refresh complete; LAi rebuilt (184 files) |
| 1355960 | `giphy_dl` downloads `Yp9qEExZm3hon67Bn1` (432 KB) |
| 1357289 | Scheduler logs **"Next periodic refresh in 37 seconds"** |
| 1368146 | `giphy_dl` downloads `E7oHJ8l4chkC5YvY2J` (1.25 MB) |
| 1378870 | `giphy_dl` downloads `l0ExayQDzrI2xOb8A` (826 KB) |
| 1388263 | `giphy_dl` downloads `OMZRxGyZZ6fGo` (870 KB) |
| 1393469 | **Second** refresh cycle fires (37 s later, as scheduled) for **Giphy Work** |
| 1393666 | Giphy Work search `q="work"` — page 1 fetch |
| 1394–1403 | Pages 2–7 fetched back-to-back (offsets 50, 100, 150, 200, 250, 300) |
| 1398494 | `giphy_dl` finishes `psZPpgNNe5TFK7gAXa` (685 KB), starts `lO6SvVyO3SHBu` |
| 1403863 | Page 8 fetch starts (`offset=350`) — TLS handshake at 1403997 |
| —       | **CRASH** during/just after that handshake (no further log line) |

`ps_pick` and `view_tracker` were running throughout. At the moment of the
crash there was an in-flight Giphy API page fetch **plus** an in-flight GIF
download (`lO6SvVyO3SHBu`), both holding TLS sessions, while the SDIO RX
path tried to grow its buffer for an inbound burst from the C6.

### Occurrence 3 — 2026-05-05 (uptime ~7172 s, ~2 h)

Same firmware as Occurrence 2 (ELF SHA256 `969b00418…`). Crash text and
register dump identical (`assert failed: sdio_rx_get_buffer sdio_drv.c:830
(*buf)`, MEPC `0x4ff0a7ce`).

Timeline of the last ~7 seconds before panic (timestamps in ms since boot):

| t (ms)  | Event |
|---------|-------|
| —       | Long stretch of normal `ps_pick`/`view_tracker` activity (no anomalies for ~3500 s prior) |
| 7165751 | `ps_refresh` cycle starts (3452 s elapsed since previous); only **Giphy Trending** needs refresh |
| 7165875 | Giphy Trending refresh begins |
| 7166052 | Page 1 (`offset=0`) — TLS handshake |
| 7167086 | Page 1 received (97.9 KB); merged at 7167212 |
| 7167379 | `channel_cache` saves Trending: 264 entries, 252 available |
| 7167392 | `dl_mgr` starts download `l0ExayQDzrI2xOb8A` (concurrent with API paging) |
| 7167412–7172528 | Pages 2–5 (`offset=50, 100, 150, 200`) fetched back-to-back |
| 7172661 | Page 6 fetch (`offset=200`) — TLS handshake |
| —       | **CRASH** during/just after that handshake (no further log line) |

Notable differences from Occurrence 2:
- Only **one** concurrent GIF download visible at crash time (vs several in Occ 2)
- Much longer uptime before crash (~2 h vs ~23 min in Occ 2)
- Both occurrences are on the *same* firmware build, so this is reproducible
  under load on a fixed binary.

### Occurrence 4 — 2026-05-05 (near-miss; no SDIO crash, ~3886 s uptime)

Same firmware as Occurrences 2 and 3 (ELF `969b00418`). System did **not**
crash, but the same load trigger surfaced as a different family of failures
in nearby subsystems, then recovered after backoff.

Timeline (timestamps in ms since boot):

| t (ms)  | Event |
|---------|-------|
| 3622734 | First refresh cycle: Giphy Trending paging starts |
| 3622879–3633699 | Trending paging (offsets 0–250) — clean |
| 3635250 | (Side issue) brief LAi=0 race after refresh; picker fell back. Tracked separately in `docs/refresh-lai-race.md` |
| 3637969 | Trending refresh complete. **Next refresh in 53 s** |
| 3691969 | Second refresh cycle: Giphy Work paging starts |
| 3694458 | `dl_mgr: All files downloaded ... waiting for signal` — **no concurrent downloads during this refresh** |
| 3692103–3786966 | Giphy Work paging (offsets 0–150) |
| 3738111 | `transport_base: esp_tls_conn_read error, errno=No more processes` (errno=11, EAGAIN) |
| 3866325 | `mqtt_client: No PING_RESP, disconnected` — Makapix MQTT keepalive timed out |
| 3869273 | Second `esp_tls_conn_read` errno=11 |
| 3884276 | `giphy_api: Giphy API read error after 32768 bytes` / `Truncated Giphy response: got 32768/96906 bytes` — page fetch had to retry |
| 3886343 | **`mqtt_client: Error create mqtt task` / `makapix_mqtt: Failed to start: ESP_FAIL`** — `xTaskCreate` returned failure on MQTT reconnect |
| 3916386 | MQTT reconnect retry (30 s backoff) |
| 3928268 | MQTT reconnected — system back to Online |

The MQTT task-creation failure at t=3886343 is the headline: `xTaskCreate`
returning failure means TCB + stack couldn't be allocated. This is the
**same internal-RAM exhaustion** that takes down the SDIO RX path; this run,
the heap pressure happened to be observed by `xTaskCreate` first instead of
`sdio_rx_get_buffer`.

### Occurrence 5 — 2026-05-14 (uptime ~6 min, with heap snapshot)

ELF SHA256: `fc8daa29f…`. ESP-IDF **v5.5.2** (compile time Mar 18 2026).
Assert text now `assert failed: sdio_rx_get_buffer sdio_drv.c:896 (*buf)`
(line shift from :830 reflects esp_hosted moving between IDF versions; same
code path, same MEPC `0x4ff0a7ce` → `panic_abort`).

**New in this occurrence:** the heap-snapshot diagnostic requested in the
"Diagnostics worth adding before deciding" section (below) has been
implemented and printed its first failure:

```
*** HEAP ALLOC FAILED (one-shot snapshot) ***
  func=heap_caps_aligned_alloc  size=9216  caps=0x0000080c  task=sdio_read
  INT+DMA+8BIT (SDIO RX): free=29863 largest=8192 total=277999
  INTERNAL:               free=67903 largest=31744
  DMA:                    free=29883 largest=8192
  SPIRAM:                 free=23409216 largest=23068672
  DEFAULT:                free=23457644 largest=23068672
*********************************************
```

Caps `0x0000080c` decodes as `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA |
MALLOC_CAP_8BIT` — DMA-capable internal SRAM, 8-bit accessible. This is
the pool every SDIO RX buffer is forced to live in (Wi-Fi-via-C6 traffic
must DMA out of internal RAM; PSRAM is not DMA-reachable for this path).

What the numbers say:
- **The pool is at 89 % utilization** (248 KB of 272 KB allocated). Peak
  network demand is grazing the ceiling.
- **And it's fragmented**: 29.9 KB free in aggregate, but the largest
  contiguous block is **8 KB**. The aligned 9 KB request can't be satisfied
  even though the pool isn't fully exhausted.
- SPIRAM has 23 MB free, but is useless here — SDIO RX needs DMA-capable
  internal RAM.

So the root cause is **both fragmentation and near-exhaustion** of the
DMA-capable internal pool. The doc previously listed candidate causes and
couldn't pick between fragmentation and absolute exhaustion; the snapshot
shows both contribute.

Timeline of the ~14 s before panic (timestamps in ms since boot):

| t (ms)  | Event |
|---------|-------|
| 345570  | Giphy "work" paging: page 1 already received (96 KB), 50 entries merged |
| 345900–359254 | Giphy "work" pages 2–10 fetched back-to-back at `offset=50…450` (~97 KB each over TLS) |
| 346077–359944 | **14 Makapix MQTT response batches** (~8.6 KB each, 32-byte cursor steps from 416 → 864 on channel `Fab` with 1027 entries) interleaved with the Giphy paging |
| 351452 | `giphy_dl` finishes `FjJlFKHKoFvS2gLeyY` (395 KB), starts `N0wPrpPdvASA0` |
| 352519 | `giphy_dl` finishes `N0wPrpPdvASA0` (16 KB), starts `9D1time2rTBAvJHyEx` |
| 353608 | `giphy_dl` finishes `9D1time2rTBAvJHyEx` (10 KB), starts `5kIh9jg6olLrZJtAf6` |
| 353609 | `channel_cache: LAi array grew to capacity 756` — incidental pressure on the aligned-internal allocator |
| 356040 | `giphy_dl` finishes `5kIh9jg6olLrZJtAf6` (108 KB), starts `U4sfHXAALLYBQzPcWk` |
| 358089 | `giphy_dl` finishes `U4sfHXAALLYBQzPcWk` (75 KB), starts `UBAf8QIWQZ7p6IOZEm` |
| 359944 | Last Makapix MQTT response logged |
| —      | **CRASH** during the next inbound burst from the C6 |

Core 1 was inside `GIFMakePels` decoding a frame at the moment Core 0
panicked (per the boot log's `Core1 Saved PC: 0x4808e884`) — playback was
healthy. The system died on the *network* side.

### Occurrence 6 — 2026-05-18 (uptime ~305 s)

ELF SHA256: `ee443f31c…` (firmware `0.10.1` per `CMakeLists.txt`).
ESP-IDF v5.5.2. Assert text identical to Occurrence 5 (`assert failed:
sdio_rx_get_buffer sdio_drv.c:896 (*buf)`, MEPC `0x4ff0a7ce`).

Heap snapshot at the moment of failure:

```
*** HEAP ALLOC FAILED (one-shot snapshot) ***
  func=heap_caps_aligned_alloc  size=10240  caps=0x0000080c  task=sdio_read
  INT+DMA+8BIT (SDIO RX): free=33731 largest=9728 total=276991
  INTERNAL:               free=73311 largest=31744
  DMA:                    free=37411 largest=9728
  SPIRAM:                 free=23198680 largest=22544384
  DEFAULT:                free=23255684 largest=22544384
*********************************************
```

Same shape as Occurrence 5: pool at **88 % utilization** (243 KB of
277 KB allocated) and fragmented — 33.7 KB free in aggregate but only
9.7 KB largest contiguous. The aligned 10.2 KB request misses by ~500
bytes.

**New trigger composition.** Earlier occurrences had Giphy paging
± GIF downloads ± Makapix MQTT batches. Occurrence 6 is dominated by
**two paginated channel refreshes overlapping** (Makapix MQTT "Fab"
+ V&A HTTPS) followed by a third paginated refresh (Wellcome HTTPS)
stacked with several concurrent downloads:

| t (s) | Event |
|-------|-------|
| 212.4 | `ps_refresh`: Makapix **"Fab"** refresh starts (1027 entries) — paginated MQTT batches |
| 212.5–244.0 | **32 Makapix MQTT batches** for "Fab" arrive at 32-byte cursor steps (preview 32 → 1024) |
| 214.7 | `ps_refresh` dispatches second slot: **V&A · Paintings** HTTPS refresh starts (`REFRESH_MAX_CONCURRENT=2` allowed the overlap) |
| 214.7–266.8 | V&A pages 1–11 fetched back-to-back over TLS (`id_category=THES48917`, ~96 KB JSON each); `esp_http_client_init`/`cleanup` per page |
| 245.0 | Fab refresh complete (1027 entries, 212 available); MQTT batches stop |
| 253.5 | `dl_mgr` starts download `I48PlTLyNTnnkdqr33` (Giphy) — TLS handshake while V&A paging continues |
| 256.1 | `dl_mgr` starts Makapix CDN download `3c3e3a11-…webp` |
| 261.2 | `dl_mgr` starts download `l41JWwvf5k0GL1SuY` |
| 266.8 | V&A refresh complete (1100 fetched, 1024 kept, 76 orphans evicted) |
| 269.2 | `ps_refresh` dispatches **Wellcome · Paintings** HTTPS refresh — same per-page TLS pattern |
| 269.2–304.1 | Wellcome pages 1–10 fetched (`genres.label=Paintings`, ~96 KB JSON each) |
| 274.0–305.1 | Five more `dl_mgr` downloads kicked off (Giphy + Makapix CDN), in parallel with Wellcome paging |
| 305.1 | `dl_mgr` starts download `TIMBLT1r15CGqxNMLt` |
| —     | **CRASH** during the next inbound SDIO RX burst from the C6 |

`ps_pick`/`ps_lai` were active throughout (channel selection, LAi cursor
advances), so the picker + animation player were under their normal
steady-state load. The system died on the *network* side, as in every
prior occurrence.

### Occurrence 7 — 2026-06-14 (uptime ~1834 s / ~30 min; **no crash, persistent livelock**)

Firmware `1.0.0` (per `CMakeLists.txt`; ELF SHA not captured — no boot
banner in this session's log). This is a **deliberate stress test**, not an
organic field failure: a 64-channel playset where every channel is a
different Makapix Club hashtag (`#wizard`, `#ghost`, `#skull`, `#heart`,
`#magic`, …). The scheduler refreshes channels *eagerly*, so 64 Makapix
MQTT channel-index refreshes march back-to-back, each followed by a
cache-save to SD and artwork downloads, while the animation player decodes
frames off SD in parallel.

**This occurrence did not crash.** The DMA-internal pool was exhausted as
in Occurrences 1–6, but the victim was the **SD card / SDMMC path** (and an
SD read on the `anim_loader` task), not esp_hosted's RX buffer. SDMMC
returns `ESP_ERR_NO_MEM` instead of asserting, so there was no panic and no
reboot — instead, every subsequent SD operation failed in a storm and the
device stayed wedged until manually power-cycled.

Heap snapshot at the moment of the first failed allocation:

```
*** HEAP ALLOC FAILED (one-shot snapshot) ***
  func=heap_caps_malloc  size=512  caps=0x00000008  task=anim_loader
  INT+DMA+8BIT (SDIO RX): free=1863 largest=496 total=264671
  INTERNAL:               free=23783 largest=10240
  DMA:                    free=1863 largest=496
  SPIRAM:                 free=24116220 largest=19398656
  DEFAULT:                free=24137456 largest=19398656
*********************************************
```

Caps `0x00000008` is `MALLOC_CAP_DMA` **alone** (contrast Occurrences 5–6's
`0x0000080c` = `INTERNAL | DMA | 8BIT`). The 512-byte request is almost
certainly the SDMMC driver's per-transfer DMA allocation, triggered by the
animation loader reading the next frame off SD — `task=anim_loader` is
whichever task happened to touch the SD bus, not a distinct consumer.

What the numbers say:
- **The pool is at 99.3 % utilization** — `free=1863` of `total=264671`.
  This is far past Occurrence 5 (89 %) and Occurrence 6 (88 %); the 64-
  channel stress test drove it lower than any organic occurrence on record.
- **Largest contiguous DMA block is 496 bytes**, so even a trivial 512-byte
  request misses — by 16 bytes. Fragmentation + near-total exhaustion, same
  signature as Occ 5/6, just deeper.
- SPIRAM has 24 MB free and is, as always, useless for this allocation.

Timeline of the ~9 s around the failure (timestamps in ms since boot):

| t (ms)  | Event |
|---------|-------|
| 1826207 | `ps_refresh` refreshes Makapix `#wizard`; index merged (5 entries) |
| 1828384 | `ps_refresh` refreshes `#ghost` (13 entries) |
| 1830561 | `ps_refresh` refreshes `#skull` (9 entries) |
| 1831834 | `dl_mgr` downloading a `#sky` pick; `ps_pick` picks `#tree` (post 3546) |
| 1832780 | `ps_refresh` refreshes `#heart` (21 entries) |
| 1834117 | `ps_pick` picks `#landscape` (post 3545) |
| ~1834400 | **HEAP ALLOC FAILED** snapshot (512 B, `MALLOC_CAP_DMA`, `anim_loader`) |
| 1834415 | `sdmmc_cmd: sdmmc_read_sectors: not enough mem, err=0x101` — SD reads start failing |
| 1834739 | `sdmmc_write_sectors` ENOMEM; `http_fetch: Write error: wrote 0/32768 bytes` |
| 1834786 | `makapix_artwork: Artwork download failed for 31c6e719-… : ESP_FAIL` |
| 1835002 | `ps_refresh` refreshes `#magic` — **MQTT/Wi-Fi still working** |
| 1835102 | `sd_path: mkdir failed: /sdcard/p3a (I/O error)` |
| 1835347 | `makapix_channel_refresh: Batch merged: ch='#magic'` — network OK, SD dead |
| 1835390 | `channel_cache: Failed to create channels directory: /sdcard/p3a/channel (errno=5)` |
| 1835391 | `makapix_channel_refresh: Cache flush failed … skipping metadata save` |
| …       | SDMMC `not enough mem` flood continues indefinitely until **manual reboot** |

The decisive detail is at t=1835347: **`#magic`'s channel index is fetched
over MQTT and merged in RAM successfully *after* the heap failure** — so
the Wi-Fi-over-SDIO path is alive and holding the DMA-internal pool, while
every SD operation (`mkdir`, cache flush, download write, frame read) fails
for want of the same pool. The scheduler keeps marching through channels
and the download manager keeps retrying, so the network side keeps the pool
pinned and the SD side never gets a window to drain. Nothing in the system
sheds load on memory pressure, so the livelock is self-sustaining.

### Pattern across all seven occurrences

The first six events share the same trigger:

1. A `ps_refresh` cycle is paging through one or more HTTPS APIs
   (Giphy, V&A, Wellcome) and/or a Makapix MQTT channel — multiple
   batches/pages of network response in quick succession.
2. The crash/symptom hits in the middle of the paging, not on the first
   page.

Occurrence 7 broadens the trigger one more step: instead of *deep* paging
of one or two channels, it's *breadth* — 64 channels each doing a small
MQTT index refresh back-to-back. Same outcome (DMA-internal pool starved),
reached by volume of refreshes rather than depth of any one.

What differs is the **victim** of the resource pressure (and, in
Occurrences 5–7, the **co-stressors** stacked on top of refresh paging):

| # | Concurrent downloads? | Other concurrent load | Symptom |
|---|----------------------|-----------------------|---------|
| 1 | yes (3 GIFs)         | —                     | SDIO RX assert (hard crash) |
| 2 | yes (multiple GIFs)  | —                     | SDIO RX assert (hard crash) |
| 3 | yes (1 GIF)          | —                     | SDIO RX assert (hard crash) |
| 4 | **no** (`dl_mgr` idle) | —                     | MQTT task-create failure + HTTP truncation + TLS EAGAIN; recovered |
| 5 | yes (5 GIFs in succession) | **heavy Makapix MQTT** (14 batches × 8.6 KB in the window) | SDIO RX assert (hard crash) |
| 6 | yes (8 in succession: Giphy + Makapix CDN) | **two concurrent paginated refreshes** (Makapix MQTT "Fab" 32 batches overlapping V&A HTTPS 11 pages, then Wellcome HTTPS 10 pages) | SDIO RX assert (hard crash) |
| 7 | yes (Makapix CDN, ongoing) | **64-channel eager refresh** (back-to-back Makapix MQTT index refreshes) + animation decode off SD | **SD path / `anim_loader` `ESP_ERR_NO_MEM`** — no crash, **persistent SD-I/O livelock until manual reboot** |

#### What Occurrence 3 newly tells us

- **Not a slow leak / cumulative fragmentation.** Occurrences 2 (23 min) and
  3 (~2 h) are on the same firmware, with vastly different uptimes. A leak
  hypothesis predicts crashes correlated with uptime; instead we see the
  crash correlated with the load *burst* (refresh + download), regardless of
  how long the device has been up. This argues for "transient peak demand
  exceeds DMA-internal heap available right now" rather than "heap slowly
  drains until it can't satisfy a normal request."
- **One concurrent download is enough.** Occurrence 2 had several large
  downloads in parallel; Occurrence 3 had just one visible. Suggests the
  refresh's own paginated TLS sessions account for most of the pressure on
  their own — even a single concurrent download tips it over.
- **Reproducibility on fixed firmware** means the next attempt to fix this
  can be evaluated against a known-failing build (no "did the firmware
  change?" confound).

#### What Occurrence 4 newly tells us

- **Refresh paging alone (zero concurrent downloads) is sufficient to push
  the system to internal-RAM exhaustion.** Previously we believed the
  trigger required refresh paging *plus* at least one concurrent download.
  Occurrence 4 disproves that for the broader resource-exhaustion problem;
  whether the SDIO assert specifically requires concurrent downloads is
  still consistent with Occurrences 1–3 but not yet falsified.
- **The "victim" of the heap pressure is non-deterministic.** Occurrences
  1–3 took down the SDIO RX path; Occurrence 4 took down `xTaskCreate` for
  the MQTT reconnect, with HTTP truncation and TLS EAGAIN as collateral.
  This means a fix that only addresses the SDIO assert leaves us exposed to
  the same root cause manifesting elsewhere — the MQTT/HTTP/task-create
  failures in Occurrence 4 happened *despite* the SDIO path coping that
  particular run.
- **`Error create mqtt task` is a useful observable proxy** for the same
  heap pressure. It's easier to reproduce on demand than the SDIO assert
  (it surfaces from refresh paging alone, no downloads needed) and produces
  a recoverable log line instead of a panic. Useful for instrumentation and
  for evaluating fixes.

#### What Occurrence 5 newly tells us

- **Fragmentation+exhaustion confirmed, no longer hypothesized.** The
  heap-snapshot diagnostic (asked for in "Diagnostics worth adding"
  below, now in firmware) prints the smoking gun at the moment of
  failure: `INT+DMA+8BIT: free=29863 largest=8192 total=277999` — the
  pool is at 89 % utilization *and* the largest contiguous block is
  smaller than the 9 KB the SDIO driver is asking for. Both
  fragmentation (item #1 in "Root cause" below) and concurrent peak
  demand (item #2) are contributing, not one or the other.
- **Makapix MQTT batches are a new co-stressor.** Occurrences 1–3 had
  Giphy refresh ± Giphy GIF downloads; Occurrence 5 adds 14 Makapix
  MQTT response batches (~8.6 KB each, every ~700 ms) running
  concurrently with Giphy paging + Giphy downloads. The trigger set is
  broader than this doc previously listed — any combination of TLS
  sessions + sustained SDIO RX traffic can hit the wall, not just the
  Giphy-specific pattern.
- **Bug is not tied to one firmware build.** Occurrences 2–4 were on
  ELF `969b00418` (IDF v5.5.1). Occurrence 5 is on ELF `fc8daa29f` (IDF
  v5.5.2, esp_hosted line shift from :830 to :896). Same fingerprint,
  different binary — rules out "this is a build artifact" and shows
  upstream hasn't fixed it in v5.5.2 either.
- **Short uptime reinforces burst-driven.** ~6 minutes from boot to
  panic in Occurrence 5 (vs ~2 h in Occurrence 3 on the older
  firmware). As soon as Wi-Fi and SD were up and the playscheduler ran
  a full refresh cycle, every condition needed to crash was met. Time
  is not a factor; *load* is.
- **PSRAM is sitting idle while the relevant pool dies.** SPIRAM had
  23 MB free at the moment of crash. None of it is reachable for the
  failing alloc (SDIO RX needs DMA-capable internal RAM), but it does
  highlight that the bottleneck pool is small and special-purpose —
  shifting *other* internal-RAM consumers to PSRAM is a separate but
  related angle worth pursuing.

#### What Occurrence 6 newly tells us

- **Museum-channel HTTPS paging is now a confirmed trigger.**
  Occurrences 1–5 were all Giphy-paging-driven (± Makapix MQTT in
  Occ 5). Occurrence 6 reaches the same wall with **V&A + Wellcome
  paging** as the dominant HTTPS workload — Giphy is reduced to a few
  concurrent downloads, not the refresh itself. The trigger class is
  "any sustained paginated HTTPS refresh," not Giphy-specific. All
  museum adapters in `components/art_institution/museums/*.c` call
  `esp_http_client_init`/`cleanup` **per page** — V&A alone did 11
  init/cleanup cycles in this run, Wellcome 10 more; identical churn
  profile to Giphy's paging.
- **`REFRESH_MAX_CONCURRENT=2` is a concrete enabler with a visible
  overlap.** The Makapix "Fab" refresh and the V&A refresh ran
  simultaneously for ~33 seconds (t=212 → t=245). Two paginated
  network refreshes were the active load at the start of the burst,
  with Wellcome and the download manager stacking on later. The
  constant is defined at
  `components/play_scheduler/play_scheduler_refresh.c:49`; lowering
  it to 1 would have eliminated the visible overlap in this run.
  Gives Option B's "stagger periodic refreshes" sub-action a concrete
  one-line handle.
- **Third firmware build hitting this fingerprint.** ELF `ee443f31c`
  (firmware v0.10.1) — distinct from `969b00418` (Occ 2–4) and
  `fc8daa29f` (Occ 5). Another data point that this isn't tied to a
  particular build; same architectural pressure, same victim.
- **Heap snapshot is consistent with Occurrence 5.** Pool utilization
  87.8 % (Occ 5: 89.3 %), largest 9728 (Occ 5: 8192), request 10240
  (Occ 5: 9216). The streaming-mode RX path is asking for
  variable-sized aligned buffers and the internal-DMA heap can't
  satisfy them under load. Same wall, slightly different exact peak.

#### What Occurrence 7 newly tells us

- **The SD path is a second victim — and it doesn't crash, it wedges.**
  All prior occurrences took down a path that *asserts* (esp_hosted
  `sdio_rx_get_buffer`) or *fails loudly and retries* (`xTaskCreate` in
  Occ 4). When the same pool exhaustion instead hits the **SDMMC driver**,
  the failure is a returned `ESP_ERR_NO_MEM`, not a panic — so the device
  keeps running with a dead SD card. This is a **strictly worse end state
  than the crash**: the assert reboots and self-heals; the livelock
  persists until a human power-cycles the board. Any fix evaluated only
  against "did it stop crashing?" can still leave this livelock in place.
- **Option A (packet mode) alone would not have saved this run.** A
  removes esp_hosted's on-demand `_h_malloc_align` growth (the panic
  path) and stops its fragmentation churn, which *helps* the SD side — but
  the steady-state demand from animation decode + TLS + per-channel cache
  writes still coexists in the same ~265 KB pool. The victim here was never
  the SDIO assert path, so removing it doesn't address the starvation. This
  is concrete evidence for the doc's existing lean toward **C (A + B)** over
  A alone, and motivates a new option (G, below): **memory-pressure
  backpressure** so any future exhaustion degrades to a graceful slowdown
  and *recovers*, instead of crashing (Occ 1–6) or wedging (Occ 7).
- **The trigger generalizes from depth to breadth.** Occurrences 1–6 were
  driven by deep paging of one or two channels (many pages/batches of a
  single refresh). Occurrence 7 reaches the same wall with 64 *shallow*
  refreshes in quick succession. The common factor is sustained
  refresh-driven network churn, regardless of whether it comes from one
  deep channel or many shallow ones. **Eager refresh of every channel in a
  playset is the engine** — a large playset multiplies the burst directly.
- **Deeper exhaustion than ever recorded, because nothing crashed to stop
  it.** Occ 5/6 bottomed at ~30 KB free / 8–10 KB largest *and then the
  device asserted*, so we never saw lower. Occ 7's graceful-degradation
  victim let the pool keep draining to `free=1863, largest=496` under
  continued load. This is consistent with the burst-driven (not slow-leak)
  conclusion from Occurrence 3: load, not uptime, sets the floor — and with
  no crash to halt the load, the floor goes lower.
- **Network survived the SD death.** `#magic`'s MQTT index refresh merged
  successfully *after* the heap failure (t=1835347), confirming the
  Wi-Fi/SDIO RX path was alive and holding the pool while SD starved. The
  two SDIO consumers (C6 link vs SD card) are genuinely competing for the
  same region, and under sustained app traffic the network side wins and
  the SD side never recovers on its own.

---

## New failure mode: SD-path starvation livelock (Occurrence 7)

Occurrences 1–6 are *crashes*: a path asserts (esp_hosted RX) or a loud,
recoverable failure fires (`xTaskCreate`). Occurrence 7 is a **livelock**:

1. The DMA-internal pool is exhausted by the same root cause (sustained
   refresh-driven network churn fragmenting + filling the ~265 KB region).
2. The allocation that fails belongs to the **SDMMC driver**, which returns
   `ESP_ERR_NO_MEM` rather than asserting. No panic, no reboot.
3. Every subsequent SD op — frame reads for decode, artwork download
   writes, `mkdir`, channel-cache flush — fails for want of the same pool.
4. Meanwhile the scheduler keeps refreshing channels (MQTT still works) and
   the download manager keeps retrying. That ongoing network traffic keeps
   the C6/SDIO RX side holding the pool, so the SD side never gets a window
   to drain.
5. **There is no load-shedding on memory pressure anywhere in the system**,
   so step 4 is self-sustaining. The device stays wedged until a human
   reboots it.

The reboot in Occurrences 1–6 was, perversely, a recovery mechanism. The
graceful SDMMC failure removes it. **The fix set must therefore include a
way to either (a) keep the pool from reaching this floor (Options A/B/E), or
(b) detect the pressure and shed load so the pool can recover (Option G).**
Without (b), a sufficiently large playset can always re-create the livelock.

---

## Root cause

`_h_malloc_align(len, 64)` returned NULL. The DMA-capable / 64-byte-aligned
internal-SRAM heap couldn't satisfy the request. Occurrence 5's heap
snapshot pins down which contributors are active simultaneously:

1. **Heap fragmentation** (confirmed). At crash time the pool had 29.9 KB
   free but a largest contiguous block of only 8 KB, and SDIO needed 9 KB
   aligned. Three TLS contexts (~16–32 KB each) had just been allocated
   and torn down for GIF downloads, another was mid-handshake for the
   Giphy API call, and Makapix MQTT was cycling its own buffers — all
   churning the same aligned pool.
2. **Concurrent peak demand** (confirmed). The pool was at 89 %
   utilization (248 KB of 272 KB) at the moment of failure. Animation
   decode buffers + LCD frame buffers + JSON parse buffer (96 KB API
   response) + multiple TLS sessions + Wi-Fi/SDIO RX path are all
   competing for the same ~272 KB region simultaneously.
3. **Streaming-mode realloc churn** (still suspected, hard to prove
   directly). Each time an unusually large RX chunk arrives, the driver
   frees the old buffer and allocates a new bigger one. This itself
   fragments the aligned heap over time and is the structural reason
   (#1) is so easy to trigger.

The streaming RX path is fundamentally fragile: it grows the buffer at
unpredictable times under unpredictable memory pressure, and a single failure
panics the whole system.

### Relevant config

```
CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_STREAMING_MODE=y
```

---

## Proposed fix (tabled)

**Switch SDIO RX mode from streaming to `MAX_SIZE`** in menuconfig:

```
Component config
└─ ESP-Hosted
   └─ SDIO Receive Optimization
      ( ) No optimization
      (X) Always Rx Max Packet size      ← change to this
      ( ) Use Streaming Mode             ← currently selected
```

This results in `CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_MAX_SIZE=y`.

### Why this fixes the crash

In `MAX_SIZE` (and `NONE`) modes, `sdio_rx_get_buffer()` uses a **mempool**
(`sdio_buffer_alloc()` → `mempool_alloc(buf_mp_g, MAX_SDIO_BUFFER_SIZE, ...)`).
Buffers are preallocated at init when the heap is fresh and unfragmented.
There's no on-demand `_h_malloc_align()` on the RX hot path, so the failing
assert path doesn't exist. If a packet ever exceeds the buffer size, the
non-streaming path drops it gracefully (logs an error, returns) rather than
panicking — see `sdio_drv.c:373` and `:415`.

### Downsides

| # | Downside | Notes |
|---|----------|-------|
| 1 | Slightly higher static internal-SRAM footprint | Mempool is permanently allocated; streaming grows on demand |
| 2 | **Lower Wi-Fi RX throughput** | Streaming reads many queued packets in one SDIO transaction; MAX_SIZE does one transaction per packet |
| 3 | Wasted SDIO bus bandwidth on small packets | MAX_SIZE always reads 1536 bytes (3×512), discards the tail — costly for ACKs and control frames |
| 4 | Higher per-byte CPU/IRQ overhead | More transactions = more interrupts, more setup work in the SDIO read task |
| 5 | Oversized packets dropped instead of regrown | If slave ever sends `len > ESP_RX_BUFFER_SIZE`, host logs ESP_LOGE and drops; TCP would retransmit. Should not happen in practice. |
| 6 | Less-traveled path for P4+C6 combo | Streaming is the upstream default; Espressif likely tunes against it |

For p3a's workload (occasional GIF downloads, MQTT keepalives, OTA, no
sustained high-bandwidth streaming), the throughput hit is probably
acceptable — but worth measuring rather than assuming.

---

## Alternative / complementary angle: tighten concurrency

Even with the config change, reducing simultaneous network pressure is good
hygiene. Candidates:

- **Cap download-manager parallelism** so we don't have 3 large GIF downloads
  running while a Giphy API refresh is also fetching.
- **Don't let an API refresh overlap with active downloads** — gate one
  behind the other.
- **Stagger periodic refreshes** so we don't get the back-to-back trigger
  pattern (Trending finishes → 10s → Work starts).
- **Stream-parse JSON** instead of buffering 96 KB API responses fully in
  RAM before parsing.

These don't eliminate the streaming-mode assert path on their own — they just
make it less likely to fire. So they're cleanup, not a fix.

---

## Diagnostics added (Occurrence 5)

The one-shot heap-failure snapshot requested here has been added to the
firmware. It hooks `heap_caps_*alloc*` failures and dumps capability-split
watermarks (`INT+DMA+8BIT`, `INTERNAL`, `DMA`, `SPIRAM`, `DEFAULT`) plus
the requesting task name and caps mask. It fired for the first time in
Occurrence 5 and produced the snapshot shown there.

Still worth adding (not yet implemented):

- **Periodic** capability-split watermark logging during normal operation,
  so we can see how close we routinely run to the wall *before* a failure
  rather than only at the moment one happens. Would help us evaluate any
  candidate fix's impact on steady-state headroom.
- `heap_caps_print_heap_info(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL |
  MALLOC_CAP_8BIT)` dump at the start and end of every refresh cycle, to
  watch the fragmentation curve over time.

---

## Decision required

Options (consolidated across occurrences):

- [ ] **A.** Switch SDIO RX mode to `MAX_SIZE`. Lowest-effort fix,
      structurally eliminates the assert path (pre-allocated mempool at
      init; oversize packets drop gracefully instead of panicking). Accept
      some Wi-Fi RX throughput loss.
- [ ] **B.** Keep streaming mode, tighten concurrency only (cap parallel
      downloads, gate refresh-vs-download, stagger periodic refreshes,
      stream-parse JSON). Reduces *probability* but the panic path still
      exists.
- [ ] **C.** Both A and B.
- [ ] **D.** ~~Add diagnostics first, decide based on data.~~ ✅ done in
      Occurrence 5.
- [~] **E. Partially landed (2026-06-02).** Reduce internal-RAM footprint
      by moving non-DMA consumers to PSRAM. An audit found the
      originally-listed targets were **already** in PSRAM: worker task stacks
      (`heap_caps_malloc(MALLOC_CAP_SPIRAM …)` + `xTaskCreateStatic`, internal
      fallback), the 96 KB–1 MB JSON response buffers (giphy / museums /
      makapix), the channel_cache LAi/Ci arrays (`psram_malloc()`), and FATFS
      SD buffers (`main/ffsystem_aligned.c` `--wrap` shim). The one major
      consumer still hitting the internal pool was **cJSON's parse tree** —
      `cJSON_InitHooks()` was never called, so every DOM node landed in
      DMA-capable internal RAM (under `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=8192`)
      during refresh paging — matching the snapshots' fragmentation + peak-demand
      signature. **Done:** `cJSON_InitHooks` now routes all cJSON allocation to
      PSRAM (internal fallback) at the top of `app_main()` (`main/p3a_main.c`),
      so JSON parse trees no longer pressure or fragment the `INT+DMA+8BIT`
      pool. **Still available, not yet applied:** raise
      `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` (currently 32 KB) and/or lower
      `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` (currently 8 KB). Doesn't fix the
      assert path; raises the ceiling. Complementary to A and B.
- [ ] **F. Lazy / just-in-time channel refresh** *(new, motivated by
      Occurrence 7).* Don't eagerly refresh every channel in a playset;
      refresh only the active + next-up channel(s), on demand. Eager
      all-channel refresh is the engine of the breadth-driven burst in
      Occ 7 — a 64-channel playset should not generate 64 near-simultaneous
      MQTT index refreshes. This attacks the trigger at the source and
      scales the fix to playset size, where B (concurrency cap) only limits
      *how many at once*, not *how many total*. Stronger version of B's
      "stagger periodic refreshes" sub-action.
- [ ] **G. Memory-pressure backpressure / load-shedding** *(new, motivated
      by Occurrence 7's livelock).* Sample the DMA-internal pool
      (`heap_caps_get_free_size` / `heap_caps_get_largest_free_block` for
      `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`); below a
      watermark, pause new downloads and channel refreshes until it
      recovers. This is the only option that addresses the **livelock**
      directly: it gives the pool the drain window it currently never gets,
      converting both the SDIO assert *and* the SD starvation into a
      graceful slowdown that recovers. Defense-in-depth that backstops
      A/B/E/F regardless of which path becomes the victim. (A cruder
      band-aid in the same spirit — remount/reset the SDMMC after a burst of
      `ENOMEM`/EIO — treats the SD symptom only and is inferior to shedding
      load before the pool bottoms out.)

The **cJSON-to-PSRAM portion of Option E has been applied** in source
(`main/p3a_main.c`, 2026-06-02). The assert-path fix (A) and concurrency
work (B) are **not** yet applied, and `sdkconfig` is unchanged.

**Current lean (revised after Occurrence 7): C + F + G** — packet mode to
remove the panic class and esp_hosted's fragmentation churn (A), lazy
just-in-time refresh to kill the burst at the source and scale with playset
size (F), and memory-pressure backpressure so any residual peak degrades
gracefully and *recovers* (G), with B's `REFRESH_MAX_CONCURRENT=1` and E's
PSRAM/knob work as cheap complements. E is pursued in parallel (its
cJSON-to-PSRAM sub-item landed 2026-06-02; the sdkconfig-knob sub-items
remain open).

Why the revision: the Occurrence 5 heap snapshot resolved the
"fragmentation vs. exhaustion" question by showing both contribute (D
closed). **Occurrence 7 then proved A is necessary but not sufficient** —
its victim was the SD path, not the SDIO assert, so removing the assert
would have left the livelock intact. Option B alone is also insufficient
(Occurrence 4 showed refresh paging by itself can exhaust the pool even
with `dl_mgr` idle; Occurrence 7 showed a large-enough playset re-creates
the burst no matter the per-cycle concurrency cap). G is the only option
that addresses the **livelock** end state directly, and is the one piece
that makes the system robust to *any* future victim of the same pool.
Concrete one-line handle from Occurrence 6 still stands: drop
`REFRESH_MAX_CONCURRENT` from 2 to 1 at
`components/play_scheduler/play_scheduler_refresh.c:49`.

---

## Tracking upstream

This is a known issue, tracked at:

- **espressif/esp-hosted-mcu#144** — *"ESP32P4 + ESP32C6 sdio_rx_get_buffer
  and transport_drv_sta_tx assert failed (EHM-156)"* — open, actively
  discussed, Espressif collaborator engaged.
- Our corroborating comment (2026-05-05) with the p3a load profile:
  https://github.com/espressif/esp-hosted-mcu/issues/144#issuecomment-4381980103
- Related thread referenced from #144:
  https://github.com/espressif/esp-hosted/issues/597 (closed; same heap-pressure
  family of bugs).
- Espressif's documented mitigation
  ("packet mode" — equivalent to RX_NONE / RX_MAX_SIZE Kconfig switch):
  https://github.com/espressif/esp-hosted-mcu/blob/main/docs/sdio.md#94-switching-to-packet-mode

Espressif's stance so far is to recommend mitigation (packet mode) rather than
patch the assert. **This recommendation is concrete and crash-specific, not
just a general pointer to the memory docs** (verified by re-reading the full
#144 thread via `gh`, 2026-06-02):

- On 2026-01-12, Espressif collaborator `SohKamYung-Espressif` first pointed at
  the general §9 memory-reduction steps — *"One solution is to reduce the amount
  of heap memory ESP-Hosted SDIO uses … at the cost of network performance"*
  ([comment](https://github.com/espressif/esp-hosted-mcu/issues/144#issuecomment-3736702205)).
- On 2026-01-13, **after analyzing the reporter's crash logs** (largest DMA block
  4608 B vs a 12288 B `sdio_rx_get_buffer` request), the same collaborator gave a
  pointed recommendation: *"Can you try switching ESP-Hosted SDIO to packet mode?
  This results in the smallest DMA allocation requested by ESP-Hosted SDIO,"*
  citing §9.4 above
  ([comment](https://github.com/espressif/esp-hosted-mcu/issues/144#issuecomment-3742476534)).

So packet mode / `RX_MAX_SIZE` is both **documented** (§9.4) and **explicitly
recommended in-thread for this exact assert** — it is the path Espressif pointed
at, not a workaround we invented. Caveats worth keeping in mind: it was framed as
*"can you try"* with no in-thread confirmation it resolved the crash (the original
reporter — a different product, with fingerprint/NFC/display — went quiet still
hunting a 100 KB+ DMA-RAM consumer), and packet mode is rationalized around
shrinking the DMA request, not as a correctness fix. Our comment makes the
explicit case for *also* replacing `assert(*buf)` with a graceful drop; no
Espressif reply to that proposal yet.

When picking this back up, check the issue thread for new replies before
deciding. The sibling-fix precedent (below) is the strongest new argument
for our position.

### Upstream developments since 2026-05-05 (checked 2026-05-16)

- **Sibling assert site fixed — but not the one that crashes us.**
  Espressif's Vikram Dattu merged `fix/sdio_rx_mempool_graceful_drop` on
  2026-04-29 — commits
  [`a914274d`](https://github.com/espressif/esp-hosted-mcu/commit/a914274d)
  ("gracefully drop packets on RX mempool exhaustion", 2026-04-03) and
  [`971cf0d8`](https://github.com/espressif/esp-hosted-mcu/commit/971cf0d8)
  ("throttle RX mempool drop log to burst start/recovery", 2026-04-23).
  The commit message describes exactly the symptom and remedy we proposed:
  *"Replace assert(pkt_rxbuff) with graceful packet skip when
  sdio_buffer_alloc returns NULL during sustained streaming. Previously
  crashed with 'assert failed: sdio_push_data_to_queue sdio_drv.c:928
  (pkt_rxbuff)' after 3-11 minutes of 1080p streaming."*

  **However:** this patch is in `sdio_push_data_to_queue`, **not** the
  `sdio_rx_get_buffer` site that crashes our build (line :830 on v5.5.1 /
  :896 on v5.5.2). Inspecting current `main` of
  `host/drivers/transport/sdio/sdio_drv.c`, the streaming-mode
  `sdio_rx_get_buffer` still has an unmodified `assert(*buf);` on the
  `_h_malloc_align` return. **Half the streaming-mode assert class was
  resolved; the half that affects us was not.** Useful precedent for our
  comment on #144 — the upstream-acceptable fix shape (assert → ESP_LOGW +
  `continue`) is now established in adjacent code.

- **No Espressif response to our 2026-05-05 comment on #144** as of
  2026-05-16 (11 days). Last activity on the issue is still our comment.

- **Independent corroboration of the budget-side issue.**
  [esp-hosted-mcu#191 "2.12.6 takes more memory than 2.12.3, cant start"
  (EHM-213)](https://github.com/espressif/esp-hosted-mcu/issues/191),
  filed 2026-05-03 (open) by an unrelated reporter, pinpoints the
  architectural change between 2.12.3 → 2.12.6: the new
  `sdio_mempool_create(tx_q_size, rx_q_size)` preallocates 31 × 1536 ≈
  47.6 KB of DMA-internal SRAM at init. Espressif collaborator
  `mantriyogesh` confirmed on 2026-05-04 that this is intentional
  (*"reverting to it is not a viable path forward… may need to reduce
  the number of mempool buckets or consider offloading some of them to
  PSRAM"*), no committed timeline. Same architectural pressure family as
  our streaming-mode assert: the DMA-internal pool is small, special-
  purpose, and increasingly contended on the P4 + C6 topology. This also
  validates the existing `~2.9.3` pin in `main/idf_component.yml` and is
  referenced from `docs/ESP-IDF-v6.0/migration-report.md` §2.6.

### Upstream re-check (2026-06-02)

- **Re-read the full #144 thread via `gh` to settle whether the packet-mode
  recommendation is crash-specific.** It is — see the two `SohKamYung-Espressif`
  comments quoted under "Espressif's stance" above. This confirms the §9.4
  citation is Espressif's actual recommendation for *this* assert, not just a
  generic memory-docs link.
- **#144 is still OPEN; no new Espressif replies.** Last activity remains our
  2026-05-05 comment; the graceful-drop proposal for `sdio_rx_get_buffer` is
  still unanswered. The thread otherwise ends in January 2026 with the original
  reporter still chasing a DMA-RAM consumer (they disabled their display and
  recovered ~80 KB of DMA RAM, confirming the pool is genuinely contended).
- **No behavioral confirmation that packet mode fixes the crash exists anywhere
  yet** — neither in-thread nor in our own builds. Our planned option-C
  build/flash/soak (see "Decision required") is what would establish it.

---

## References

Line numbers below are from IDF v5.5.1 (Occurrences 1–4). On IDF v5.5.2
(Occurrences 5–6) the failing assert is at `sdio_drv.c:896` and surrounding
ranges have shifted by ~60 lines; same code paths, different offsets.

- Failing assert: `managed_components/espressif__esp_hosted/host/drivers/transport/sdio/sdio_drv.c:830` (v5.5.1) / `:896` (v5.5.2)
- Streaming-mode RX path: `sdio_drv.c:810–835` (v5.5.1)
- Non-streaming-mode RX path (uses mempool): `sdio_drv.c:766–782` (v5.5.1)
- Buffer mempool: `sdio_drv.c:210–232` (v5.5.1)
- Kconfig choice: `managed_components/espressif__esp_hosted/Kconfig:549–575`
- Project SDIO bus coordinator (unrelated to this crash, but adjacent code):
  `components/sdio_bus/sdio_bus.c`
