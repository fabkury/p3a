# A 2.0 skeleton

Part of the [2.0 architecture review](README.md). This page is the
constructive half: what the firmware looks like if the findings on the other
four pages are applied, and where the four announced 2.0 changes (A single
binary, B 192-byte records, C repetition suppression, D aspect-ratio filter)
land in it. It is a proposal for discussion, not a plan with dates. Names of
new components are placeholders.

Constraints taken as fixed: the Waveshare ESP32-P4 4B board only, ESP-IDF
5.5.x, C, vanilla HTML/JS served from the device, PICO-8 removed, nothing
carried over from 1.x (a 2.0 flash is a fresh device).

## 1. Layers and the one rule

```
main         app_main() and nothing else
  |
net          wifi, captive portal, http server + routes, mqtt commands, ota, mdns, sntp
  |
ui           screen model, ugfx screens, overlays, touch router, intro animations
  |
player       scheduler (SWRR, pick, history, repetition ring), loader pipeline,
             decoders, upscalers, display pipeline (producer/consumer/vsync)
  |
library      record store (indices), blob store, download manager, refresh manager,
             eviction; owns every record and every byte on the card
  |
sources      makapix, giphy, klipy, museum (9 adapters), local, pinned
             each implements source_ops_t; none includes anything above library
  |
storage      sd_path, fs_atomic, sd_health, sd_repair, settings store, fs_caps
  |
board        display, touch, SD mount, button, brightness, USB MSC, co-processor flash
  |
util         psram_alloc, limits, error base, task helper, job runner, mem pressure
             no IDF application dependencies; compiles on the host
```

The rule: an arrow only points down. Every arrow is a `REQUIRES`. No
`WHOLE_ARCHIVE`, no `__attribute__((weak))`, no component includes a header
from `main/`. Upward notification goes through two mechanisms only: the event
bus for fan-out (swap requests, connectivity changes, storage health) and a
hook table that `main` fills at boot for the few places a lower layer needs to
ask the UI to draw something.

This is also the test boundary. `util`, `storage`'s codecs, `library`'s index
logic, the scheduler's pick and SWRR, the repetition ring, the aspect predicate
and the settings schema have no IDF includes and compile on the host under a
small FreeRTOS stub. A GitHub Actions job builds the firmware and runs those
tests on every push.

## 2. The data model

Three types carry the whole content side.

**`record_t`, 192 bytes, packed.** One per known artwork in a channel index.

```
common header (64 B):  source u8, kind u8, extension u8, flags u8,
                       id32 (hash or server id), sort_at u32, modified_at u32,
                       width u16, height u16, resolve_fails u8, download_fails u8,
                       blob_ref (sha256 prefix, 16 B), reserved
identity union (128 B): makapix { uuid[16] }
                       giphy   { id[32] }
                       klipy   { id u64, product u8 }
                       museum  { museum_id u16, iiif_key[96] }
                       local   { filename[120] }
```

The discriminator is in the record. `width` and `height` are filled by the
loader on first decode. `blob_ref` is the content hash the blob store is keyed
by, zero until downloaded. The pinned list is not a record layout of its own;
a pin is a `record_t` copied from its source channel plus a `refs` entry.

**`index_t`.** A framed container on disk and a PSRAM array in memory:
`{magic, schema, version, record_size, count, flags, crc32}` then records,
then an open-addressed `uint16_t` slot table (8 KiB at 4096 records) and an
availability bitmap (512 bytes). One loader, one saver, one "rebuild on
mismatch" rule. The channel caches, the local-file scan, the playsets, the
active-playset snapshot and the pin orders are all instances. At 2048 records
per channel it is 384 KiB plus 5 KiB, and because the uthash nodes are gone
it is smaller than today's 64-byte channel. Sixteen resident channel indices
at a time is the budget; more channels in a playset load on demand.

**The blob store.** `blobs/{d0}/{d1}/{sha256-prefix}.{ext}` plus a `refs`
index of `{blob_ref, ref_count, pinned}`. Every downloaded or uploaded byte
goes through `http_fetch_to_file` or the upload handler into it, hashed on the
way. Pinning bumps a ref and sets `pinned`; eviction never touches a pinned
blob and otherwise ages by mtime exactly as today. LAi is the availability
bitmap, maintained when a blob lands or is evicted, never by `stat()` at pick
time.

## 3. The content loop

```
refresh manager (one task)       for each channel due: source->refresh() -> merge into index_t
download manager (one task)      for each record without a blob, round-robin: source->url() -> http_fetch -> blob store -> bitmap
scheduler (called on the player) SWRR over channels -> pick over available bitmap -> repetition penalty -> aspect predicate -> record
loader (one task)                record -> blob path -> decode first frame into a staging buffer -> swap_ready
render (display producer)        adopts staging buffer at a frame boundary -> swap_shown
```

`swap_shown` is the only event that commits history, `last_played_id`, the
view tracker and the repetition ring. A record that fails to load never
enters history. Failures carry a project error enum (`NOT_PLAYABLE`,
`CORRUPT`, `MISSING`, `RATE_LIMITED`, `AUTH`, `NO_KEY`, `SD_FAILED`); the
loader deletes a blob only on `CORRUPT`, and only through the blob store.

Makapix refresh runs on the refresh task like every other source: it publishes
a page request over MQTT and blocks on a queue the MQTT handler feeds, with a
timeout. The per-channel task, the pending table and the poll loop are gone.

One `refresh_state_t` per channel (`IDLE`, `DUE`, `RUNNING`, `BACKOFF(until)`,
`BLOCKED(reason)`) replaces the four booleans plus streak and deadline fields.

## 4. Where A, B, C and D land

**A. Single binary.** `webui/` is gzipped at build time and embedded with
`EMBED_FILES`. `net` serves from a static table with `Content-Encoding: gzip`
and an `ETag` derived from the firmware version. The captive portal serves its
pages from the same table. Deleted: the web UI OTA engine, LittleFS, the
surrogate, the health check, `compat.js`, `metadata.json`, `version.txt`,
`WEBUI_VERSION`, `P3A_API_VERSION`, the `storage` partition, two release
assets and the `webui` block of `manifest.json`. One version number remains.
Measured size: about 150 KB after the PWA icons are shrunk, in an 8 MB slot
whose current occupant is 2.46 MB. The partition table below says what the
freed space does.

### Partition table

Decided 2026-11-17. A 2.0 flash is a serial reflash anyway, so the table is
reordered once; after release it is fixed for the life of 2.x.

| Partition | Type | Offset | Size | Role |
|---|---|---|---|---|
| `nvs` | data | 0x9000 | 64 KB | settings blob, Wi-Fi and Makapix secrets |
| `phy_init` | data | 0x19000 | 4 KB | radio calibration |
| `otadata` | data | 0x1A000 | 8 KB | boot slot and rollback state |
| `slave_fw` | data 0x40 | 0x20000 | 2 MB | ESP32-C6 image for `slave_ota` |
| `coredump` | data | 0x220000 | 256 KB | crash dumps |
| `ota_0` | app | 0x260000 | 10 MB | app slot A |
| `ota_1` | app | 0xC60000 | 10 MB | app slot B |
| `spare` | data | 0x1660000 | 9.6 MB (to end of flash) | reserved, unformatted, no code behind it |

Changes from 1.x: the 4 MB LittleFS `storage` partition is gone; the
fixed-size partitions move to the front so that the two most likely to be
resized later (the app slots and the spare block) share one boundary; the
app slots grow from 8 to 10 MB; the 9.6 MB that 1.x never allocated plus
the former `storage` space become one contiguous `spare` data partition.
The first three partitions keep their 1.x offsets so NVS survives a reflash
on dev units.

Reasoning. The embedded UI and every other asset on the 2.0 list (fonts,
logos, intro animations, museum tables) fit in a few MB; 10 MB slots cover a
4x growth of today's image. `spare` has no consumer; its value is that a
data partition can be mounted later without a reflash while an app slot
cannot be grown without one. Do not ship LittleFS or any filesystem code
for it until something needs it. `slave_ota` locates its partition by
subtype, and the web flasher reads offsets from `flash_args`, so the
reorder needs no code change.

Rules that go with it: a CMake check fails the build when `p3a.bin` exceeds
90 percent of the slot, because `esp_https_ota` otherwise rejects the image
only at install time on the device; every byte embedded in the binary is
downloaded and flashed by every device on every update, so embed sparingly
and gzip text assets; and any `partitions.csv` edit triggers an `sdkconfig`
regeneration that must keep the silicon-revision guard intact.

**B. 192-byte records.** Section 2. The size is right, the identifier limits
that produced the CMA and Wellcome workarounds and the Library of Congress
deferral go away, and the memory cost is lower than today once the per-node
hash allocations are gone. The playset `identifier` widens to 96 bytes in the
same change.

**C. Suppress repetitions.** A RAM-only ring of `{id32, source, shown_at}` in
the player, fed by `swap_shown`, sized for a few hours of plays. The random
pick weights each available record by `1 - penalty(age_since_shown)` with a
floor so nothing is ever excluded, then samples. One function in the picker,
one ring, no per-source code, reset by reboot. With one pick function instead
of three, the change is small; a host test can check the distribution.

**D. Filter aspect ratio.** The predicate is `w * 10 > max_tenths * h` on
the record's `width` and `height`, applied in the picker to museum records
only, failing open when either is zero, exempting pinned and "show this"
exactly as `docs/deferred/aspect-ratio-filter.md` specifies. The loader fills
the fields on first decode and returns `NOT_PLAYABLE`, which the pipeline
treats as "pick again" without touching retries, history or the blob. The
traps that page lists are structural in 1.x and absent here.

## 5. Settings

One schema file in the repo, `{key, type, default, min, max,
reboot_required, group, label}`. The build generates the C table and the
typed struct; the UI loads the same file at runtime to render and validate
the settings page. The store holds the struct in RAM behind a mutex, reads
NVS once at boot, writes one blob on change. Adding a setting is one line in
the schema. Wi-Fi credentials and Makapix secrets stay in their own NVS
namespaces.

## 6. The HTTP surface

One route table under `/api/v2/`. Settings only through `/api/v2/config`.
One playset activation verb. Jobs for anything slower than a few
milliseconds, with `GET /api/v2/jobs/{id}` for progress; firmware OTA install
is a job. One `send_ok` that applies the envelope. `ETag` on every resource
GET. A `player_commands` module that both the HTTP handlers and the Makapix
MQTT handler call, so the two transports are one implementation.

In the UI: one `static/api.js`, page scripts moved out of the HTML into
`static/*.js` so they gzip and cache separately, numeric limits taken from
the settings schema rather than typed twice, and a `museums.json` shared by
the browser adapters and compiled into the device adapters so the two cannot
drift.

## 7. Tasks

About 25 tasks, created through one helper that encodes priority, core and
PSRAM-by-default from a class (`RENDER`, `TOUCH`, `APP`, `NETWORK`,
`BACKGROUND`). Two long-lived job runners (I/O and UI) absorb the twelve
one-shot tasks. A DMA-pool watermark in `util` is read by the download
manager, the refresh manager and `http_fetch`, which back off when it is
low. Networking stays on core 0 and the display pipeline on core 1, as the
jitter work established.

## 8. What carries over unchanged

The display pipeline, the jitter fixes, the decoders, `http_fetch`,
`fs_atomic`, `sd_health`, `sd_repair`, `sd_path`, `frame_trace`,
`mem_stats`, the SWRR core with credit rollback, the loud/silent failure
split, the museum adapters' listing logic, the Makapix MQTT and provisioning
flow, the Wi-Fi manager and captive portal logic, `slave_ota`, the boot intro
animations, and the ugfx screens as drawing code. Roughly, the parts that talk
to hardware or to servers are fine; the parts that connect them to each other
are what 2.0 rebuilds.

## 9. What 2.0 deletes outright

PICO-8 (`components/pico8`, `http_api_pico8.c`, `webui/pico8/`,
`playback_controller.c`), the web UI OTA stack, LittleFS, `content_cache`,
`playback_queue`, `channel_interface.h` and its three implementers,
`sdcard_channel_impl.c`, `makapix_channel_switch.c`, `makapix_single_artwork.c`,
`playlist_manager.c`, `channel_settings.c`, the legacy `.bin` channel index
path, the three forwarding services, the eight dead Kconfig options, the
fifteen inherited Wi-Fi Kconfig entries, the hex image arrays (replaced by
`EMBED_FILES`), and every `__attribute__((weak))`.

## 10. Order of work, if it helps

Not a schedule, just dependencies. `util`, `storage` and the settings schema
first, because everything else includes them and because the host test
harness needs them. Then `library` with the record and blob stores, with the
six sources moved behind `source_ops_t` one at a time (local first, Makapix
last). Then the player's loader pipeline rewrite around `swap_ready` and
`swap_shown`, which is where C and D go in. Then `net`: embedded UI (A), route
table, jobs. The UI model consolidation can happen any time after the
hook table exists. Each step leaves a device that boots and plays.
