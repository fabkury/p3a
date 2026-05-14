# PSRAM migration opportunities

**Status:** Open. Generated from a codebase sweep on 2026-05-14 following
Occurrence 5 of the SDIO RX OOM crash (`docs/sdio-rx-oom-crash.md`).
**Goal:** raise the ceiling on internal-SRAM pressure by moving non-DMA
allocations to PSRAM. This is a complementary workstream to the
SDIO-mode Kconfig switch tracked in the OOM-crash doc — it does *not*
eliminate the SDIO assert path, but it makes the bottleneck pool less
crowded, reducing the rate at which any of the failure modes from
Occurrences 1–5 fire.

---

## Headline

The codebase is already in pretty good shape. Most of the obvious large
consumers — Giphy/museum API response buffers (96–384 KB), MQTT
reassembly (128 KB), animation frame buffers (1.5–5 MB), JPEG/PNG/PICO-8
frame buffers, and most large task stacks — are already using the
PSRAM-first-with-internal-fallback pattern, often via the helper at
`components/channel_manager/include/psram_alloc.h` (`psram_malloc`,
`psram_calloc`, `psram_strdup`). The `download_manager`
(`components/channel_manager/download_manager.c:821`) is the
gold-standard exemplar — PSRAM stack via `xTaskCreateStaticPinnedToCore`
with dynamic-internal fallback.

What's left is mostly **smaller-but-numerous** consumers that just
defaulted to plain `malloc()` / `xTaskCreate` without anyone having
retrofitted them. Aggregate steady-state savings estimated at **40–80 KB
of internal RAM**, plus 12 KB from cert `.bss`, plus whatever Wi-Fi/LwIP
buffers can be shifted via Kconfig. On a 272 KB pool running at 89%
utilization (per the Occurrence-5 snapshot), that's the difference
between "fragile" and "comfortable."

**The failing allocation itself (SDIO RX, `MALLOC_CAP_INTERNAL |
MALLOC_CAP_DMA | MALLOC_CAP_8BIT`) cannot move to PSRAM** — Wi-Fi-via-C6
traffic needs DMA-accessible internal SRAM. This work raises the
*ceiling* on the bottleneck pool; it does not eliminate the assert
path. Pair it with option A or C from `docs/sdio-rx-oom-crash.md`.

---

## What's already on PSRAM (no action)

| Category | Files | Status |
|---|---|---|
| Giphy / museum / GitHub API JSON responses (96–384 KB each) | `giphy_refresh.c:327`, `artic.c:356`, `loc.c:378`, `rijksmuseum.c:314,591`, `vam.c:312`, `smk.c:325`, `wellcome.c:364`, `github_ota.c:223,410` | PSRAM-first w/ internal fallback |
| MQTT reassembly buffer (128 KB) | `makapix_mqtt.c:180` | PSRAM-first |
| Download chunk buffers (32 KB each) | `giphy_download.c:234`, `makapix_artwork.c:233`, `art_institution_download.c:75`, `show_url.c:434`, `pin_lists_copy.c:48` | PSRAM-first |
| Animation/PICO-8/decoder frame buffers (~1.5–5 MB) | `animation_player_loader.c:908,928`, `png_animation_decoder.c:166,179`, `jpeg_animation_decoder_sw.c:148`, `pico8_render.c:111` | PSRAM-first |
| Large task stacks (4–80 KB) | `download_manager`, `animation_sd_refresh`, `event_bus_dispatch`, `api_worker`, `ota_check`, `wifi_recovery`, `wifi_health_monitor`, `ps_refresh`, `show_url`, `makapix_channel_switch`, `makapix_mqtt_reconnect` (static path), `makapix_credentials_poll`, `view_tracker`, `makapix_channel_refresh` | PSRAM-stack pattern |

---

## Tier 1 — High-impact, low-risk opportunities

### A. Move task stacks that currently use plain `xTaskCreate` to the PSRAM pattern

Aggregate ~60–90 KB of internal RAM in task stacks that never attempt
PSRAM. Reference pattern: `download_manager.c:816–826`.

| Task | Stack | File:line | Notes |
|---|---|---|---|
| `app_touch_task` | 8 KB | `main/app_touch.c:644` | Touch input poll loop; not ISR-touched, runs forever |
| `mqtt_reconnect` (duplicate path) | 16 KB | `components/makapix/makapix_connection.c:47` | Identical purpose to the `makapix.c` path; that one does PSRAM, this one doesn't |
| `makapix_mqtt_reconnect` (dynamic-fallback path) | 16 KB | `components/makapix/makapix.c:253` | Static-PSRAM path at line 490 exists; verify both paths take it |
| `makapix_provisioning` | 8 KB | `components/makapix/makapix.c:223` | Run-once at provisioning |
| `pin_io` | 8 KB | `components/p3a_core/p3a_pin_dispatcher.c:226` | Recently bumped from smaller (commit `3c77c57e fix(pin_lists): bump pin_io task stack to 8 KB`) |
| `giphy_click` | 8 KB | `components/p3a_core/p3a_reaction_dispatcher.c:193` | One-shot reaction handler |
| `display_upscale_worker_top/bottom` | unknown | `main/display_renderer.c:188,201` | **Verify size first** — if large, may need PSRAM but ALSO need a perf check; render hot path |
| `display_render` | unknown | `main/display_renderer.c:274` | Same caveat |
| `touch_init` / `touch_rescue` | 4 KB / 2.5 KB | `main/app_touch.c:516,539` | Small — borderline worth it |
| `reaction_mqtt` | 4 KB | `components/p3a_core/p3a_reaction_dispatcher.c:106` | Small |
| `status_pub` | 4 KB | `components/makapix/makapix_connection.c:157` | Small |
| `dns_server` | 4 KB | `components/wifi_manager/wifi_captive_portal.c:649` | Only during captive portal |

**Pros:** Direct savings; pattern proven in-tree; surgical per task.

**Cons:** PSRAM has higher access latency than internal SRAM (~10–20×
slower for cache misses). Hot-path tasks doing many fine-grained stack
accesses can take a measurable perf hit. For the tasks above (mostly
event/blocking loops), this won't matter. **The two display-renderer
tasks need a stack-size measurement and a perf check before moving** —
they're on the render hot path and may justifiably want to stay
internal.

**Risk:** Some FreeRTOS port internals may rely on stack being in
internal RAM for ISR-context handling. The in-tree precedents
(`download_manager`, `animation_sd_refresh`, `ps_refresh`, etc.) have
run reliably with PSRAM stacks, so the pattern is validated for
application-level tasks. **Do not move** tasks that have ISR callbacks
executing on their stack.

**Recommendation:** Do the non-renderer ones in one PR. Investigate the
renderer ones separately with stack-size + frame-time measurement
first.

### B. Move the three MQTT cert arrays to PSRAM `.bss`

`components/makapix/makapix_mqtt.c:48–50`:

```c
static char s_ca_cert[4096]   = {0};   // 4 KB
static char s_client_cert[4096] = {0}; // 4 KB
static char s_client_key[4096]  = {0}; // 4 KB
```

Total: **12 KB of internal `.bss` for the lifetime of the device**, read
only at TLS handshake time. ESP-IDF MQTT client stores pointers
(doesn't copy), so the buffers must remain valid — but they don't need
to be in internal RAM.

**Pros:** Surgical, 12 KB recovered, near-zero risk. Cert reads happen
at TLS handshake start, not in any DMA or ISR path.

**Cons:** The clean idiom is `EXT_RAM_BSS_ATTR` annotation, which
requires `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`. Enabling that
Kconfig affects all annotated buffers globally — minor blast radius
but worth verifying nothing else regresses. Alternative: leave Kconfig
alone and convert to one-time heap allocation with `MALLOC_CAP_SPIRAM`
at MQTT init (the cleaner option if you don't want Kconfig churn).

**Recommendation:** Do this. The heap-alloc approach is the safer
path; it doesn't touch Kconfig at all.

### C. Apply the existing `psram_alloc.h` helper to plain-`malloc` JSON/text buffers

Plain `malloc()` for transient parse buffers — total peak might be
30–80 KB depending on workload.

| Buffer | Typical size | File:line | Lifetime |
|---|---|---|---|
| channel_metadata JSON | per file | `components/channel_manager/channel_metadata.c:150` | parse-and-free |
| config_store JSON | small | `components/config_store/config_store.c:97` | parse-and-free |
| channel_settings JSON | small | `components/channel_manager/channel_settings.c:38` | parse-and-free |
| playlist_manager JSON | playlist size | `components/channel_manager/playlist_manager.c:644` | parse-and-free |
| pin_lists_manifest | manifest size | `components/pin_lists/pin_lists_manifest.c:33` | parse-and-free |
| http_api_utils JSON | request size | `components/http_api/http_api_utils.c:87` | parse-and-free |
| http_api_rest_settings | request size | `components/http_api/http_api_rest_settings.c:318` | parse-and-free |
| sdcard_channel_impl JSON | per file | `components/channel_manager/sdcard_channel_impl.c:165` | parse-and-free |
| play_scheduler_playsets entries | **N × entry_size, can be 25 KB+ for big channels** | `components/play_scheduler/play_scheduler_playsets.c:180` | channel lifetime |
| play_scheduler_lai available_post_ids | **N × 4 bytes, ~4 KB for the 1027-entry Fab channel** | `components/play_scheduler/play_scheduler_lai.c:128` | channel lifetime |
| WebP still_rgba / still_rgb | W×H×4 / W×H×3 | `components/animation_decoder/webp_animation_decoder.c:152,171` | decode-and-free |
| PNG row_pointers | small | `components/animation_decoder/png_animation_decoder.c:193` | decode-and-free |
| wifi_captive_portal HTML | small | `components/wifi_manager/wifi_captive_portal.c:130,156,182` | request lifetime |

**Pros:** Helper already exists (`psram_alloc.h`). Most are trivial
mechanical changes (`malloc` → `psram_malloc`). The play_scheduler
ones are the highest-value targets in this list — long-lived and grow
with channel size; the crash log noted "channel_cache: LAi array grew
to capacity 756" right before the panic.

**Cons:** PSRAM is slower; for `play_scheduler_lai.c` the array is
accessed frequently during picker iteration, so a benchmark is
warranted before moving that one (though picker latency is dominated
by other things and 4 KB fits in cache).

**Recommendation:** Bulk PR for all the safe ones. For
`play_scheduler_lai`, profile first.

---

## Tier 2 — `sdkconfig` knobs

### D. Enable `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`

Currently `# CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP is not set`
(`sdkconfig:1550`).

This lets LwIP buffers (TCP segments, mbox entries, pbuf pool) try
PSRAM first. Plausibly tens of KB recovered.

**Important: on the ESP32-P4 + ESP32-C6 esp_hosted topology, the Wi-Fi
driver itself runs on the C6 — the P4 host sees only LwIP + SDIO
transport.** The failing SDIO RX buffers will remain internal because
they're explicitly allocated with `MALLOC_CAP_DMA`. So enabling this
knob shifts the *application-side* LwIP buffers off internal RAM
without touching the DMA-required SDIO path.

**Pros:** No code change. Frees internal RAM for the DMA path. LwIP
buffer access isn't perf-sensitive enough to feel PSRAM latency.

**Cons:** Slight per-packet latency increase from PSRAM-resident
pbufs. Some older LwIP allocation paths may assume internal-only;
unlikely in modern IDF but worth a grep.

**Recommendation:** Enable and measure. Probably the single
highest-impact change in this whole report, since LwIP buffers are
otherwise invisible to application code.

### E. Bump `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` from 32 KB to 64–96 KB

Currently `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768`
(`sdkconfig:1551`). Reserves internal RAM that the SPIRAM-preferring
allocator will not fall back to — guaranteeing DMA-required allocs
always have at least that much headroom.

Given the failing allocation was 9 KB and the pool was 89% used at
crash time, 32 KB of reserved headroom was clearly not enough in
practice. SPIRAM has 23 MB free, so making SPIRAM-fallback bail
earlier costs essentially nothing.

**Pros:** No code change. Directly addresses the "internal RAM is
full" half of the root cause. Cheap.

**Cons:** Slightly more SPIRAM-fallback failures for plain `malloc()`
that exceeds remaining SPIRAM (irrelevant here, SPIRAM has 23 MB).

**Recommendation:** Bump to 64 KB initially. Watch steady-state heap
behavior.

### F. Enable `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`

Currently disabled (`sdkconfig:1552`). Required for `EXT_RAM_BSS_ATTR`
to work — i.e., for opportunity B (cert arrays) if you take that
route.

**Pros:** Enables a clean idiom for "this static buffer lives in
PSRAM." Useful for any future large static buffers.

**Cons:** Less-traveled IDF path. May surface bugs in libraries that
didn't expect external `.bss`.

**Recommendation:** Skip if doing B via the heap-alloc route.

### G. mbedTLS — leave alone

`CONFIG_MBEDTLS_DYNAMIC_BUFFER=y` plus `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`
already route TLS in/out buffers (16 KB IN + 4 KB OUT per session) through
the default allocator. With `SPIRAM_MALLOC_ALWAYSINTERNAL=8192`, the 16 KB
IN buffer lands in PSRAM (above threshold) and the 4 KB OUT buffer lands
in internal RAM (below threshold). Lowering `SSL_OUT_CONTENT_LEN` from
4096 to 2048 would save a few KB per concurrent session but probably
isn't worth the change.

---

## Tier 3 — Do not move

- **SDIO RX buffers** — `MALLOC_CAP_DMA` required; this is literally what's failing
- **OTA install chunk** (`ota_manager_install.c:38`, 4 KB, `MALLOC_CAP_DMA | MALLOC_CAP_8BIT`) — reads partition sectors via SPI flash DMA
- **Upscale lookup tables X/Y** (`animation_player_loader.c:800,807`) and **PICO-8 lookup tables** (`pico8_render.c:152,162`) — accessed from upscale worker tasks on the render hot path without lock protection; PSRAM latency would visibly impact frame rate
- **mbedtls_sha256_context** on stack (`ota_manager_install.c:36`) — fine as stack-local
- **Anything with `IRAM_ATTR` / interrupt callbacks** — must remain in internal SRAM by definition

---

## Recommended sequencing

1. **Kconfig sweep (one PR)**: Enable `SPIRAM_TRY_ALLOCATE_WIFI_LWIP`,
   bump `SPIRAM_MALLOC_RESERVE_INTERNAL` to 65536. Soak under typical
   load. Use the now-instrumented heap snapshot to confirm headroom
   improved.
2. **`psram_malloc` retrofit (one PR)**: Apply the helper to all
   plain-`malloc` sites in opportunity C (skip
   `play_scheduler_lai.c:128` pending profile). Low blast radius.
3. **Task-stack retrofit (one PR)**: Move the no-PSRAM-attempt task
   stacks (opportunity A) to the static-PSRAM pattern. Skip the two
   display-renderer tasks until you've measured their stack-size and
   frame-time impact.
4. **Cert arrays (one PR)**: Either heap-alloc-at-init (clean, no
   Kconfig) or `EXT_RAM_BSS_ATTR` + Kconfig flip.
5. **Display-renderer tasks (defer or skip)**: Investigate separately
   with a frame-time benchmark.

Combined, this is option **E** in `docs/sdio-rx-oom-crash.md` —
complementary to options A/B/C there, not a replacement.

---

## Tracking

- `docs/sdio-rx-oom-crash.md` — the originating crash family this
  workstream supports.
- `components/channel_manager/include/psram_alloc.h` — the
  in-tree helper for the retrofit.
- `components/channel_manager/download_manager.c:816–826` — the
  reference pattern for PSRAM-backed task stacks.
