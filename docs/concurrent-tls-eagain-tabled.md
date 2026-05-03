# Concurrent TLS EAGAIN During Giphy Refresh

Status: **Option 2 implemented** (2026-05-03). Truncated-read retry with backoff is now in `giphy_download.c` and `giphy_api.c::giphy_fetch_page`. Options 3 and 4 remain on the shelf — revisit if Option 2 retries turn out to be frequent in real usage.

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

### Option 4 — TLS semaphore with N=2 (hybrid)

Shared semaphore around `esp_http_client_perform` (or equivalent) capped at 2 concurrent TLS sessions. Preserves the API-fetch + binary-download overlap that cold-start latency depends on, but blocks a third concurrent session (view_tracker pingback, OTA check, second download) from piling on during a refresh burst.

**Pros**
- Caps worst case structurally rather than hoping buffers absorb it.
- Preserves the cold-start pipelining that ruled out Option 1.

**Cons**
- More invasive than Option 2 (~30 lines plus a shared handle, threaded through every HTTPS call site).
- Overkill if Option 2 alone keeps retries rare in practice.

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
