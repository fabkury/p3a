# SDIO RX buffer OOM crash (esp_hosted streaming mode)

**Status:** Open. Decision tabled — not yet implemented.
**First observed:** 2026-05-04.
**Last observed:** 2026-05-05 (fourth occurrence; see "Occurrence 4" — a
near-miss, no crash, but same root cause family).
**Severity:** Hard crash + reboot. **Now confirmed recurring** — three hard
crashes and one near-miss in two days, all triggered by Giphy refresh paging
under load. Occurrences 2, 3, and 4 are on the same firmware
(ELF `969b00418`), so the bug is reproducible-under-load on a fixed build.

---

## TL;DR

The ESP32-P4 panics with an unconditional assert inside the esp_hosted SDIO
driver when it can't allocate a growable RX buffer for traffic from the
ESP32-C6 Wi-Fi co-processor. The assert fires under simultaneous Giphy refresh
+ active GIF downloads + animation playback. The cleanest fix is a Kconfig
change to switch the SDIO RX mode from streaming to `MAX_SIZE`
(preallocated buffer), trading some Wi-Fi throughput for stability.

---

## Crash details

```
assert failed: sdio_rx_get_buffer sdio_drv.c:830 (*buf)
Core 1 register dump:
MEPC    : 0x4ff0a7ce  (panic_abort)
MCAUSE  : 0x00000002  (illegal instruction — the unimp from panic_abort)
```

Failing line in
`managed_components/espressif__esp_hosted/host/drivers/transport/sdio/sdio_drv.c:830`:

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

### Pattern across all four occurrences

All four events share the same trigger:

1. A `ps_refresh` cycle is paging through a Giphy endpoint (multiple ~96 KB
   JSON responses over TLS, in quick succession).
2. The crash/symptom hits in the middle of the API paging, not on the first
   page.

What differs is the **victim** of the resource pressure:

| # | Concurrent downloads? | Symptom |
|---|----------------------|---------|
| 1 | yes (3 GIFs)         | SDIO RX assert (hard crash) |
| 2 | yes (multiple GIFs)  | SDIO RX assert (hard crash) |
| 3 | yes (1 GIF)          | SDIO RX assert (hard crash) |
| 4 | **no** (`dl_mgr` idle) | MQTT task-create failure + HTTP truncation + TLS EAGAIN; recovered |

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

---

## Root cause

`_h_malloc_align(len, 64)` returned NULL. The DMA-capable / 64-byte-aligned
heap couldn't satisfy the request. Most likely causes, in rough order of
probability:

1. **Heap fragmentation.** Three TLS contexts (~16–32 KB each) had just been
   allocated and torn down for the GIF downloads, and another one was
   mid-handshake for the Giphy Work API call. Aligned allocation requires a
   contiguous block.
2. **Concurrent peak demand.** Animation decode buffers + frame buffers + JSON
   parse buffer (96 KB API response) + multiple TLS sessions + SDIO RX path
   all want internal SRAM at the same time.
3. **Streaming-mode realloc churn.** Each time an unusually large RX chunk
   arrives, the driver frees the old buffer and allocates a new bigger one.
   This itself fragments the aligned heap over time.

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

## Diagnostics worth adding before deciding

Before committing to the config change, it would help to confirm the OOM
hypothesis with data:

- Periodic logging of free-heap watermarks split by capability (internal
  SRAM / DMA-capable / PSRAM). Today we don't log this regularly, so we don't
  know how close we routinely run to the wall.
- Heap-trace or `heap_caps_print_heap_info()` snapshot during a refresh
  cycle to see fragmentation in the aligned/internal heap.

If watermarks show plenty of headroom, the issue is fragmentation rather
than absolute exhaustion — both fixes (config switch, concurrency tightening)
still help, but it changes how aggressive the concurrency cleanup needs
to be.

---

## Decision required

When picking this back up, decide between:

- [ ] **A.** Switch SDIO RX mode to `MAX_SIZE`. Lowest-effort fix, eliminates
      the assert path. Accept some Wi-Fi throughput loss.
- [ ] **B.** Keep streaming mode, tighten concurrency only. Reduces
      probability but the panic path still exists.
- [ ] **C.** Both A and B.
- [ ] **D.** Add diagnostics first, decide based on data.

No fix has been applied. `sdkconfig` and source are unchanged.

After the second occurrence, option **D (diagnostics first)** is weaker: we
already have two data points showing the same fingerprint. Leaning toward
**A** (config switch) or **C** (A + concurrency tightening), unless someone
wants to instrument first to confirm fragmentation vs. absolute exhaustion
before touching Kconfig.

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
patch the assert. Our comment makes the explicit case for replacing
`assert(*buf)` with a graceful drop. No commitment from upstream yet.

When picking this back up, check the issue thread for new replies before
deciding.

---

## References

- Failing assert: `managed_components/espressif__esp_hosted/host/drivers/transport/sdio/sdio_drv.c:830`
- Streaming-mode RX path: `sdio_drv.c:810–835`
- Non-streaming-mode RX path (uses mempool): `sdio_drv.c:766–782`
- Buffer mempool: `sdio_drv.c:210–232`
- Kconfig choice: `managed_components/espressif__esp_hosted/Kconfig:549–575`
- Project SDIO bus coordinator (unrelated to this crash, but adjacent code):
  `components/sdio_bus/sdio_bus.c`
