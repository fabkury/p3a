# Concurrent TLS EAGAIN During Giphy Refresh

Status: **Options 2, 3 and 4 implemented.** Truncated-read retry with backoff (Option 2) landed 2026-05-03 and was later centralized in `components/http_fetch`. Option 3 (raise lwIP recv mailboxes) landed 2026-06-15 (`24f05172`). Option 4 (TLS concurrency gate) landed 2026-06-15 after the mailbox bump alone still left large downloads losing every retry under a refresh burst — see "Option 4" below for the as-built notes.

Update 2026-06-05: the retry/truncation core has since been centralized in the shared `components/http_fetch` helper, and the truncation check was extended to chunked transfer-encoding (no Content-Length) — see the dated bullet under Option 2's implementation notes.

## Symptom

During a periodic Giphy channel refresh, the log shows:

```
E (...) transport_base: esp_tls_conn_read error, errno=No more processes
W (...) HTTP_CLIENT: esp_transport_read returned:0 and errno:11
...
I (...) giphy_api: Received 2742 bytes from Giphy API
E (...) giphy_api: Failed to parse Giphy JSON response (2742 bytes)
W (...) giphy_refresh: Page fetch failed at offset=96: ESP_FAIL
...
E (...) mqtt_client: No PING_RESP, disconnected
W (...) makapix_mqtt: Disconnected
```

`errno=No more processes` is newlib's legacy strerror text for **errno 11 = EAGAIN** ("Resource temporarily unavailable"). The historical SysV name (`fork()` returning EAGAIN when out of process slots) is what newlib still prints; it has nothing to do with processes here.

The system self-heals: the previous Giphy cache (e.g. 286 entries) stays active, the failed page is retried on the next refresh cycle, MQTT auto-reconnects, and playback never stalls. User-visible cost is roughly 30 s of "Connecting…" in the status indicator and one stale page of trending GIFs until the next refresh.

## Diagnosis

The refresh fans out into several overlapping TLS sessions on the same Wi-Fi link, with no concurrency gate.

Timeline from the captured log:

| ts (ms) | event |
|---|---|
| 10335980 | periodic refresh starts |
| 10337687 | GIF binary download kicks off (`dl_mgr`) right after page 1 is merged |
| 10337865 | Giphy page 2 fetch starts — while the download is still in flight |
| 10337946 / 10338117 | two more `Certificate validated` lines back-to-back → multiple TLS handshakes simultaneously |
| 10360991 | another cert validated (more concurrency piling on) |
| 10376561 / 10388343 | `esp_tls_conn_read … errno=11` × 2 |
| 10403346 | page 3 returns only **2742 bytes** of JSON instead of ~93 k → "Failed to parse" — the body got truncated mid-stream because of the failed reads |
| 10419625–10448359 | makapix MQTT publish times out 3× (10 s each) |
| 10455394 | `No PING_RESP, disconnected` — broker dropped the session |

### Why EAGAIN propagates as a hard read failure

`esp_http_client` calls `esp_tls_conn_read` → `mbedtls_ssl_read` → lwIP socket read. When lwIP returns `-1 / EAGAIN`, esp-tls logs the error and bubbles `0` back up. The HTTP client treats `read==0` as end-of-stream, which is how 2742 bytes were accepted as "the whole response" and JSON parsing failed. There is no retry loop above this layer for partial bodies.

### Where the EAGAIN comes from

- `sdkconfig` has `CONFIG_LWIP_MAX_SOCKETS=32` (fine), but the lwIP TCPIP recv mailbox and pbuf pool are shared across all sockets. Under 4–5 concurrent TLS streams plus MQTT, the receive mbox can fill, and a socket whose recv-mbox is full returns EAGAIN even on a blocking read.
- mbedTLS sessions also each carry a ~16 KB in/out content buffer (we have `MBEDTLS_DYNAMIC_BUFFER=y`, `ASYMMETRIC_CONTENT_LEN=y`). Concurrent sessions stack memory, but the harder cap is the lwIP queue, not heap.

### Concurrency is not gated

- `components/giphy/giphy_api.c` opens a fresh `esp_http_client` per page with no global lock.
- `components/giphy/giphy_refresh.c:472` signals the download manager after each page merge, so the downloader's HTTPS GET starts while the next page request is still in flight.
- `components/channel_manager/download_manager.c` runs on its own task (core 0, prio 3); makapix MQTT and `view_tracker` are also prio 3. There is no shared HTTP/TLS semaphore.

### Why MQTT also broke

The MQTT broker connection wasn't itself failing — its keepalive PING got starved out for >30 s while the Giphy + downloader storm was holding lwIP resources, so the broker timed out the session server-side.

## Options Considered

### Option 1 — Defer the download trigger until the page loop finishes

Move the `download_manager_rescan()` call out of the per-page loop so it fires only after the full refresh completes. Eliminates the page-fetch + binary-download overlap at zero RAM cost.

**Rejected.** The pipelined trigger is intentional: it lets the first images appear before the full cache is built. Killing the overlap visibly hurts cold-start UX on large channels with no previously cached artwork — exactly the case where the user is most likely to be staring at the screen waiting.

### Option 2 — Retry on truncated read **(implemented 2026-05-03)**

In `giphy_fetch_page` (and analogous call sites), compare bytes received against `esp_http_client_get_content_length()`. If short, free, back off briefly, refetch the page. Small backoff schedule (e.g. 1 s → 3 s → give up) so a still-contended retry doesn't tight-loop.

**Implementation notes:**
- `giphy_download.c` and `giphy_api.c::giphy_fetch_page` wrap the network section in a 3-attempt loop with backoff `{0, 1000, 3000}` ms.
- Truncation detected when: `esp_http_client_read` returns `< 0`, `total == 0`, or `total < Content-Length`.
- Fatal (no retry): 404 in download path; 401/403/429 in API path; oversized (>16 MiB); local file write/truncate error; response buffer overflow.
- Coverage gap: `view_tracker` pingbacks, `register_click`, `fetch_random_id`, and the makapix HTTPS paths still lack retry. They were skipped because (a) view_tracker is one-shot best-effort, (b) the others fire infrequently and weren't in the observed failure window. Extend by following the same pattern if needed.
- **2026-06-05 update:** the retry/truncation core has since been centralized in
  `components/http_fetch` (`do_fetch()`), which giphy, all seven museums,
  `makapix_artwork`, and `show_url` now call — Option 2 coverage is much wider
  than the original two call sites (the bullets above predate that refactor;
  of the listed gaps, `view_tracker` events go over MQTT today, and the
  remaining direct-`esp_http_client` paths are `register_click` /
  `fetch_random_id` (click-driven, best-effort), OTA, provisioning, and
  `makapix_promoted_https` — the last is flagged in `docs/v1.0-readiness-audit.md`).
  Same date, the truncation check was extended to **chunked transfer-encoding**:
  a 200 body whose terminating zero-length chunk never arrived is now treated
  as a truncated read and retried (log: `"Truncated chunked response"`).
  Previously the EAGAIN→`read==0` mapping described above was indistinguishable
  from EOF when the server sent no Content-Length, so a partial chunked body
  was accepted as success — in the file path it got renamed into the vault as a
  corrupt artwork that only the decode-time corrupt-file deletion
  (`animation_loader_try_delete_corrupt_cached_file`, 60 s cooldown, fires only
  on loud decode failures) could catch later; truncated-but-decodable
  animations were never caught at all. Close-delimited responses (no
  Content-Length, not chunked) remain undetectable at this layer by design:
  `esp_http_client_is_complete_data_received()` always reports false for them,
  so the new check is gated on `esp_http_client_is_chunked_response()`.

**Pros**
- Localized (~20 lines in `giphy_fetch_page`).
- Defensive against more than just this race — Wi-Fi blips and server hiccups also produce premature `read==0`.
- No memory cost, no sdkconfig churn.
- Composable with any other option.

**Cons**
- Treats the symptom, not the cause. Concurrent TLS contention still occurs; refresh latency creeps up if it worsens.
- Retry can hit the same wall during the failure window — needs backoff.
- Only fixes one call site. `view_tracker`, `giphy_download`, and `makapix_api` HTTPS paths have the same exposure; consistent coverage means a shared helper.
- Doesn't help MQTT keepalive starvation.

### Option 3 — Bump lwIP recv resources

sdkconfig knobs only: `CONFIG_LWIP_TCPIP_RECVMBOX_SIZE`, `CONFIG_LWIP_TCP_RECVMBOX_SIZE`, `CONFIG_LWIP_PBUF_POOL_SIZE`, `MEMP_NUM_TCP_SEG`.

**Pros**
- Zero code change.
- Helps every concurrent path: view_tracker, downloader, MQTT keepalive, future OTA.
- Cheap on ESP32-P4 with PSRAM.
- Reduces the MQTT-disconnect failure mode too.

**Cons**
- Raises the ceiling, doesn't remove it. With more concurrency or weaker Wi-Fi we'd hit the new ceiling.
- Some buffers live in internal SRAM, not PSRAM (e.g. `LWIP_PBUF_POOL_SIZE`, Wi-Fi RX buffers). Worth measuring `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` before/after.
- Bottleneck can shift silently to the Wi-Fi driver pools (`CONFIG_ESP_WIFI_*_BUFFER_NUM`), making future debugging harder.
- Hides the bug from future-you. No log breadcrumbs when it does happen.

### Option 4 — TLS semaphore with N=2 (hybrid) **(implemented 2026-06-15)**

Shared semaphore around `esp_http_client_perform` (or equivalent) capped at 2 concurrent TLS sessions. Preserves the API-fetch + binary-download overlap that cold-start latency depends on, but blocks a third concurrent session (view_tracker pingback, OTA check, second download) from piling on during a refresh burst.

**Pros**
- Caps worst case structurally rather than hoping buffers absorb it.
- Preserves the cold-start pipelining that ruled out Option 1.

**Cons**
- More invasive than Option 2 (~30 lines plus a shared handle, threaded through every HTTPS call site).
- Overkill if Option 2 alone keeps retries rare in practice.

**Implementation notes (as built):**
- The `~30 lines threaded through every call site` cost evaporated: since the 2026-06-05 refactor every content fetcher (giphy pages + downloads, all seven museums, makapix_artwork, show_url) funnels through `http_fetch`'s `do_fetch()`, so a single counting semaphore there gates all of them. The gate lives entirely in `components/http_fetch/http_fetch.c` — `tls_gate()` (lazy, thread-safe create via create-outside-critical + compare-and-set) plus a take/give pair.
- **N is `CONFIG_HTTP_FETCH_MAX_CONCURRENT_TLS`** (new `components/http_fetch/Kconfig`, default 2, range 1–8). Set to 1 to fully serialize `http_fetch` transfers without a code change.
- **Slot scope = live network work only.** Acquired after the SDIO wait and the redirect-scratch alloc (which do no network); released once below the hop loop. The slot is handed back during inter-attempt backoff sleeps and re-taken after, so a retrying transfer doesn't pin a slot while idle. There are no early `return`s between take and give, so a slot can't leak.
- **Not gated (by design / out of scope):** the persistent Makapix MQTT-over-TLS link (separate esp-mqtt stack — it benefits indirectly from the reduced HTTP contention, which directly addresses the keepalive-starvation failure mode), and the few remaining direct-`esp_http_client` paths that bypass `http_fetch`: giphy `register_click` / `fetch_random_id` (click-driven, best-effort), OTA, and provisioning. These are infrequent and weren't in the observed failure window; route them through `http_fetch` if they ever need gating.
- **No deadlock risk:** the gate is a leaf resource (no nested gate take, and the download_manager mutex is explicitly *not* held across the fetch), and waiters block with `portMAX_DELAY` — which is the intended backpressure, not a hang.
- Does **not** by itself give a stalled large download forward progress on retry (each retry still restarts at byte 0; there's no HTTP Range/resume). The gate's job is to stop the stall from happening; Range-resume remains an independent, composable follow-up if large downloads still occasionally exhaust their attempts.

## Recommendation Going In

If/when this is picked up: start with **Option 2** ✅ done, add **Option 3** as a complementary rate-reducer so retries fire rarely. Escalate to **Option 4** only if Option 2 retries turn out to be frequent in real usage.

## Trigger to Revisit (escalate to Option 3 / Option 4)

Reasons to take Options 3 or 4 off the shelf:
- Frequent "Retrying %s in ..." log lines from `giphy_dl` or `giphy_api` (means Option 2 is firing often, not just absorbing rare blips).
- Download retries hitting the 3-attempt cap and giving up regularly.
- Frequent "Connecting…" flashes reported by users.
- MQTT keepalive disconnects recurring during refresh bursts (Option 2 doesn't help MQTT).
- Adding more channels, OTA-during-playback, or any new persistent HTTPS path that increases steady-state concurrency.

## Key Code References

- `components/giphy/giphy_refresh.c:472` — per-page download trigger
- `components/giphy/giphy_api.c` — `esp_http_client_init` per page, no lock
- `components/giphy/giphy_download.c` — binary download HTTPS
- `components/channel_manager/download_manager.c` — downloader task (core 0, prio 3)
- `components/makapix/view_tracker.c` — pingback HTTPS on view events
- `components/makapix/makapix_mqtt.c` — persistent MQTT-over-TLS
- `sdkconfig` — `CONFIG_LWIP_MAX_SOCKETS`, `CONFIG_LWIP_TCP_RECVMBOX_SIZE`, `CONFIG_MBEDTLS_DYNAMIC_BUFFER`, `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN`
