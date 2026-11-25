# Known issues

Open problems in the shipping firmware and web UI, one entry each. An entry
leaves this page when its fix ships; the investigation history stays in git.
Ideas that were evaluated and parked on purpose live in `docs/deferred/`.

Last reviewed 2026-10-01 against `main` (v1.2.3 plus unreleased work).

## SDIO RX: internal-RAM exhaustion

**Symptom.** Under heavy network churn the ESP32-P4 panics with
`assert failed: sdio_rx_get_buffer sdio_drv.c:<line> (*buf)` inside
esp_hosted and reboots. Seen seven times between 2026-05-04 and 2026-06-14,
across several builds and IDF 5.5.1/5.5.2: five crashes, one near miss
(MQTT `xTaskCreate` failed instead), and one stress test (64 Makapix hashtag
channels) where the SD driver was the victim and the device sat in a
persistent SD-I/O failure livelock until power-cycled. Triggers seen: Giphy
refresh paging plus GIF downloads, museum HTTPS paging, and many Makapix
channel-index refreshes back to back.

**Root cause.** The DMA-capable internal SRAM pool
(`MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT`, about 272 KB) is
both nearly full and fragmented at peak load. The heap snapshot from the
fifth occurrence showed 89% use and a largest free block of 8 KB against a
9 KB aligned request. In streaming RX mode
(`CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_STREAMING_MODE=y`, still selected)
esp_hosted grows its RX buffer on demand with `_h_malloc_align()` and asserts
if that fails. Concurrent TLS sessions, MQTT buffers and refresh bursts all
churn the same pool. SDMMC fails the same way but returns `ESP_ERR_NO_MEM`
instead of asserting, and nothing in the firmware sheds load on memory
pressure, which is how the livelock sustains itself.

**Mitigations in place.**

- Option E, partial: `cJSON_InitHooks()` at the top of `app_main()` routes
  every cJSON parse tree to PSRAM (`main/p3a_main.c`). Most large buffers and
  worker stacks were already PSRAM-first.
- `heap_diag_alloc_failed_hook` in `main/p3a_main.c` prints a one-shot
  capability-split heap snapshot on the first failed allocation of the boot.
- The `http_fetch` TLS gate (`CONFIG_HTTP_FETCH_MAX_CONCURRENT_TLS=2`) caps
  concurrent HTTPS transfers, which reduces churn but does not remove the
  assert path.
- Untested assumption: the `sd_health` latch, which shipped after the
  livelock was observed, would probably trip on such a storm of failed
  writes, stop downloads and show the SD error until reboot. That would
  replace the livelock with a misleading "SD card failed" message.

**Options still open.** Nothing below is applied; `sdkconfig` still has
streaming mode, `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768`, and
`REFRESH_MAX_CONCURRENT` is 2 in
`components/play_scheduler/play_scheduler_refresh.c`.

- **A.** Switch to packet mode (`CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_MAX_SIZE=y`).
  RX buffers come from a mempool allocated at init and oversize packets are
  dropped, so the assert path disappears. Costs some Wi-Fi RX throughput,
  to be measured. Espressif recommended exactly this for this assert in
  [esp-hosted-mcu#144](https://github.com/espressif/esp-hosted-mcu/issues/144)
  (still open; our proposal to replace the assert with a graceful drop is
  unanswered). Necessary but not sufficient: it does not prevent the SD-side
  starvation.
- **C.** A plus concurrency tightening: `REFRESH_MAX_CONCURRENT` to 1, keep
  refreshes from overlapping downloads, stagger periodic refreshes.
- **E.** Finish raising the internal-heap ceiling:
  - `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` (currently unset), so LwIP
    buffers on the P4 side can live in PSRAM;
  - `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` from 32 KB to 64 KB;
  - move the three 4 KB Makapix PEM arrays (`s_ca_cert`, `s_client_cert`,
    `s_client_key` in `components/makapix/makapix_mqtt.c`) to a one-time
    PSRAM allocation;
  - move the task stacks still allocated from internal RAM (touch, Makapix
    provisioning and reconnect, `pin_io`, `giphy_click`, status publish,
    captive-portal DNS) to the PSRAM-stack pattern used by
    `download_manager`; measure the display-renderer tasks before touching
    them;
  - switch the remaining plain-`malloc` parse buffers (channel metadata and
    settings, playlist manager, pin-list manifest, HTTP API bodies) to
    `psram_malloc`. Profile `play_scheduler_lai.c` before moving its array.
- Also considered: lazy refresh of only the active and next channels instead
  of every channel in the playset (the cause of the 64-channel burst), and
  load shedding that pauses downloads and refreshes while the DMA pool is
  below a watermark. Shedding is the only option that addresses the
  livelock directly.

**Next step.** Choose a combination; the last recorded lean was A + C with E
as a cheap complement, plus lazy refresh and load shedding. Any choice needs
a build and an on-device soak under a full refresh cycle. Note also that
esp_hosted stays pinned at `~2.9.3` (`main/idf_component.yml`): newer lines
preallocate about 47.6 KB of this same pool
([esp-hosted-mcu#191](https://github.com/espressif/esp-hosted-mcu/issues/191)).

## Slow software JPEG decodes

**Symptom.** Some museum JPEGs take 45 to 105 seconds to decode. The screen
stays on the previous artwork for that long.

**Root cause.** The IDF 5.5 hardware JPEG decoder rejects progressive files
and dimensions it cannot handle, and IIIF `!720,720` requests return
aspect-preserving sizes (720x703, 478x720, ...) that often land there. Those
files go through libjpeg-turbo on the CPU.

**What shipped.** A wallclock-gated `jpeg_progress_mgr` callback yields every
200 ms from inside libjpeg, so the IDLE1 task watchdog no longer fires
(`components/animation_decoder/jpeg_animation_decoder_sw.c`). The dwell timer
skips ticks while the loader is busy and the scheduler rolls back SWRR credit
on rejected picks (`components/play_scheduler/play_scheduler_timer.c`,
`play_scheduler_navigation.c`).

**Open.**

- Bound the decode: a wallclock budget in the progress monitor that aborts
  through the existing setjmp/longjmp path with a distinct error code
  (exempt from corrupt-file deletion), plus a skip list so the same image
  does not stall again on every pick. Land a per-decode log line
  (duration, dimensions, progressive flag) first to size the budget.
- `anim_loader` is created unpinned (`main/animation_player.c`). Pinning it
  to CPU 0 was deferred: it would share the core with `download_mgr`.
  Revisit if a new watchdog trace shows CPU 1 saturation the progress
  monitor cannot reach (for example in libwebp, which does not yield).
- `CONFIG_ESP_TASK_WDT_PANIC` stays off, so any residual watchdog hit is a
  warning, not a reset. Do not turn it on without closing the libwebp case.

## Open findings from the 2026-06 code audit

A multi-agent review of v0.10.2 (2026-06-02) produced about 90 findings. Every
item below was re-checked against `main` on 2026-10-01 and is still open.
Fixed since the audit and dropped here: the Giphy page heap overflow and the
captive-portal DNS stack overflow (`5cd491b7`), the unbounded `POST /rotation`
read, broken API-version negotiation, playset deletion on version mismatch,
undetected truncated chunked downloads (now caught in `http_fetch`), and the
hand-rolled HTTPS client in `makapix_promoted_https.c`. The audit's one
remaining blocker is the SDIO entry above. IDs are the audit's, kept so
commits can cite them.

### Higher severity

| ID | Problem | Where | Fix direction |
|----|---------|-------|---------------|
| H1 | JPEG width/height only checked for 0; `aligned_w * aligned_h * 3` can wrap in 32-bit `size_t`, so a crafted file can overrun the HW-path buffer (SW path too) | `animation_decoder/jpeg_animation_decoder.c`, `jpeg_animation_decoder_sw.c` | Reject dimensions above a maximum; compute sizes in 64-bit **Fixed in 2.0** (`v2`, M2.1); still open in 1.x |
| H2 | `channel_cache_save` reads the cache arrays without `cache->mutex` while a merge can free them | `channel_manager/channel_cache.c` (`channel_cache_flush_all` path) | Snapshot under the mutex, write outside it |
| H3 | Eviction's LAi cleanup loop dereferences `evicted_ids` with no NULL guard after a failed allocation | `channel_manager/channel_cache_evict.c` (~line 99) | Guard with `if (evicted_ids)` |
| H4 | `pending_find()` returns the node after releasing `s_pending_mutex`; a timed-out waiter can free it while the response callback writes to it | `makapix/makapix_api.c` (~line 153) | Match, write and give under the mutex |
| H5 | Touch-init timeout deletes `ctx.done_sem` (stack-allocated `ctx`) while `touch_init_task` may still use both | `main/app_touch.c` | Heap-allocate the context and let the task free it |
| H6 | `dns_gethostbyname()` gets a pointer to a stack flag; the caller gives up before lwIP's DNS timeout, so a late callback writes a dead frame | `p3a_core/p3a_state_connectivity.c` | Use `getaddrinfo()` or a static context with a request token |
| H8 | Firmware OTA: early `esp_https_ota_begin` / `get_img_desc` failures skip `ota_exit_ui_mode()` and `p3a_state_exit_to_playback()`, leaving the OTA screen up | `ota_manager/ota_manager_install.c` | Mirror the later cleanup |
| H9 | Web UI OTA marks the partition invalid before the partition lookup and size check, forcing a needless recovery download on failure | `ota_manager/ota_manager_webui.c` | Set the flag just before the erase |
| H10 | `ESP_ERROR_CHECK` on `makapix_init`, `app_lcd_init`, `app_usb_init`, `connectivity_service_init` reboots on recoverable failures | `main/p3a_main.c` (`app_main`) | Log and degrade, or show the fatal-error UI |
| H11 | `showToast()` is an empty stub, so every save confirmation and error in the playset editor is silent | `webui/playset-editor.html` (~line 2034) | Real implementation, ideally shared (see M29) |
| H12 | PICO-8 `fbView` is captured once; WASM heap growth detaches it and frames go black | `webui/pico8/pico8.js` | Re-derive when `fbView.buffer !== Module.HEAPU8.buffer` |
| H13 | Disconnect handler `vTaskDelete`s the status-publish task, which may be inside `esp_mqtt_client_publish()` | `makapix/makapix_connection.c` | Let the task exit on a flag |
| H14 | Public RGBA `animation_decoder_decode_next` has no callers and writes `W*H*4` into buffers sized `W*H*3` | `animation_decoder/include/animation_decoder.h` and the four decoders | Delete the RGBA entry point **Fixed in 2.0** (`v2`, M2.1); still open in 1.x |
| H16 | REST routes span three naming eras with no version prefix; `GET /api/museum/rate-limits` skips the `{ok,data}` envelope | `http_api/http_api.c`, `http_api_rest_museum.c` | Decide and document the frozen surface |
| H18 | `pin_lists.h` still documents v1 behavior (post_id 0, monotonic ids); the code rejects `original_post_id <= 0` | `pin_lists/include/pin_lists.h` | Rewrite the header for v2. **Fixed in 2.0** (`v2`, M4.4): rewritten as `p3a_pins.h` |
| H19 | No build CI and no tests; the only workflow publishes to Pages | `.github/workflows/` | A build-on-push job, then host tests for pure logic |

### Medium severity

| ID | Problem | Where |
|----|---------|-------|
| M1 | `ps_peek_next_available` removes LAi entries during a "non-advancing" peek | `play_scheduler/play_scheduler_pick.c` |
| M2 | Channel display-name lookups iterate `s_state.channels` without `s_state.mutex` | `play_scheduler/play_scheduler.c` |
| M3 | Cache load trusts on-disk `ci_offset` / `lai_offset` without checking them against the file size | `channel_manager/channel_cache.c` |
| M4 | `artwork_modified_at` is set to `time(NULL)` instead of parsing the stored value | `channel_manager/playlist_manager.c` |
| M5 | `config_store` has no mutex; concurrent read-modify-writes of the config blob lose settings | `config_store/config_store.c` |
| M6 | `/static/*` and `/museum/*` map the URI to a path with no `..` check | `http_api/http_api_pages.c` |
| M7 | Upload response puts the raw filename into JSON unescaped | `http_api/http_api_upload.c` **Fixed in 2.0** (`v2`, M2.3): the name goes through the JSON builder; still open in 1.x |
| M8 | PICO-8 audio stop clears `s_task` after a fixed 100 ms delay, no handshake | `pico8/pico8_audio.c` |
| M9 | Event bus calls handlers while holding its dispatch mutex | `event_bus/event_bus.c` |
| M10 | Parallel upscaler waits for both worker bits with no overall deadline | `main/display_upscaler.c` **Fixed in 2.0** (`v2`, M2.1): the wait has a 2 s deadline; still open in 1.x |
| M11 | `show_url` hands off a request through unlocked globals and a binary semaphore; a queued URL can be dropped | `show_url/show_url.c` **Fixed in 2.0** (`v2`, M4.4): each request is a job with its own copy of the address |
| M12 | Fresh-boot NVS erase list includes the unused `p3a_boot` and omits `ota` | `p3a_core/fresh_boot.c` (~line 149) |
| M13 | HAM and Smithsonian BYOK keys go into URLs unencoded | `art_institution/museums/ham.c`, `smithsonian.c` |
| M14 | AIC POST DSL inserts `term_id` into the Elasticsearch body without validation (AIC is currently disabled) | `art_institution/museums/artic.c` |
| M15 | Web UI OTA state fields written without the mutex `webui_ota_get_status` takes | `ota_manager/ota_manager_webui.c` |
| M16 | `pin_list_unpin` filters `order.bin` by `post_id`, which can collide | `pin_lists/pin_lists.c` **Fixed in 2.0** (`v2`, M4.4): a record is found by its whole identity, and the API names an entry by position and key together |
| M18 | `schedulePollSoon` does not track its timeout; overlapping calls stack pollers | `webui/index.html` |
| M19 | Rijks browse fires all set fetches at once, and its cursor loop does not stop on an empty page | `webui/museum/rijksmuseum.js` |
| M20 | NVS namespaces are an undocumented contract; `ota` keys are macros in one file and literals in another | `ota_manager_webui.c`, `p3a_board_ep44b/p3a_board_fs.c` |
| M21 | Partial: both playset stores now keep files on version mismatch, but the two formats are not documented together | `playset_store.c`, `active_playset_store.c` **Fixed in 2.0** (`v2`, M2.3): one format for both, files kept on any failed check; still open in 1.x |
| M22 | Config "atomic" write: `cfg_new` is never consulted on boot, so it only doubles flash writes; header still says atomic | `config_store/config_store.c`, `config_store.h` |
| M23 | Partial: app rollback is enabled, but there is no anti-rollback, no real post-boot self-test, no minimum-version gate | `ota_manager/ota_manager_install.c`, `sdkconfig` |
| M24 | Task WDT posture is undocumented: PANIC off, libwebp does not yield (see "Slow software JPEG decodes") | `sdkconfig` |
| M25 | `p3a_render_set_channel_message` takes an `int`; several callers pass bare literals | `p3a_core/include/p3a_render.h` and callers **Gone in 2.0** (`v2`, M3.1): the UI is one typed model; still open in 1.x |
| M26 | OPC `set_rotation` reconfigures the framebuffer on the MQTT event task | `makapix/makapix_mqtt.c`, `makapix_opc.c` |
| M27 | Provisioning and OTA fields in `s_render` are written and never read | `p3a_core/p3a_render.c` **Gone in 2.0** (`v2`, M3.1): the UI is one typed model; still open in 1.x |
| M28 | Board header `#define`/`#undef`s `CONFIG_LCD_PIXEL_FORMAT_*` symbols Kconfig owns | `p3a_board_ep44b/include/p3a_board.h` |
| M29 | Web UI helpers (`showToast`, `escapeHtml`/`escHtml`) are copied per page; no shared `common.js` | `webui/*.html` |

### Low severity

- **Dead code:** network-diagnostics stubs in `http_api.c`; `webp_decoder_*`
  externs in `static_image_decoder_common.h`; unused `cached_item_t`
  (`content_cache.h`) and WebP union fields; legacy `animation_player` API
  (`find_animations_directory`, `get_next_asset_index`,
  `animation_player_add_file`); `DISPLAY_UPSCALE_SINGLE_WORKER` branch;
  `pico8_render_mark_consumed()`; unreachable `.tmp` filter in
  `playset_store_list`.
- **Duplication:** inline LAi shift-remove copies beside `lai_remove_entry`
  (`channel_cache.c`, `channel_cache_merge.c`); file-extension detection x3
  (Makapix); two User-Agent builders (`museums/common.c`, `artic.c`); two
  GitHub release walks (`github_ota.c`); cached-scalar getters in
  `config_store`; per-overlay corner mapping; captive-portal page serving x3;
  inline SDIO wait in `giphy_download.c` instead of `wait_on_sdio`.
- **Comment drift:** `channel_cache.h` says v20 and "currently 1" (version is
  25); `giphy.h` says post ids are negative (they are masked positive);
  `makapix_artwork.c` says 64 KB above a 32 KB define; `config_store.h` says
  `channel_select_mode` defaults to 0 (code returns 1); `cfg_get_string`
  returns `NOT_FOUND` after copying the default, contrary to its header;
  `p3a_current_post.c` "single writer" note ignores the reaction flag.
- **Hardening:** `channel_cache_deinit` leaks `lifecycle_mutex`; OTA
  `failure_count` is `uint8_t` and wraps; OTA SHA buffer needlessly
  `MALLOC_CAP_DMA`; version-parse failure compares as equal; slave OTA
  `fw_size` not clamped to the partition; `MIN_AGE_HOURS` Kconfig has no
  range (0 spins the eviction loop); `sdio_bus_is_locked()` takes the mutex
  to test it; `STATIC_IMAGE_FRAME_DELAY_MS` has no fallback; touch PSRAM stack
  not freed on fallback; the OOM snapshot hook fires once per boot;
  `p3a_state` callbacks run outside the mutex; NVS SSID buffer cannot
  round-trip a 32-character SSID.
- **Web UI:** upload filename into `innerHTML`; PICO-8 stream WebSocket never
  reconnects; PICO-8 input listeners never removed.
- **Structure:** nine museum adapters repeat the same refresh skeleton (no
  shared driver); Wellcome and SMK ignore `channel_offset` silently and wrap
  math differs per museum; eight browse modules duplicate `getJson`, 429
  reporting and `mapWithConcurrency`; no written policy on which persisted
  formats are disposable caches and which are user data.
