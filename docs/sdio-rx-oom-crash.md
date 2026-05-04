# SDIO RX buffer OOM crash (esp_hosted streaming mode)

**Status:** Open. Decision tabled — not yet implemented.
**First observed:** 2026-05-04 (single occurrence in monitor log).
**Severity:** Hard crash + reboot. Frequency unknown (only seen once so far).

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

---

## References

- Failing assert: `managed_components/espressif__esp_hosted/host/drivers/transport/sdio/sdio_drv.c:830`
- Streaming-mode RX path: `sdio_drv.c:810–835`
- Non-streaming-mode RX path (uses mempool): `sdio_drv.c:766–782`
- Buffer mempool: `sdio_drv.c:210–232`
- Kconfig choice: `managed_components/espressif__esp_hosted/Kconfig:549–575`
- Project SDIO bus coordinator (unrelated to this crash, but adjacent code):
  `components/sdio_bus/sdio_bus.c`
