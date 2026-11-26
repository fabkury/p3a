# Architecture

p3a 2.x runs on an ESP32-P4 (two RISC-V cores, 32 MB PSRAM, 32 MB flash) with an ESP32-C6 for Wi-Fi, reached over SDIO. It is built on ESP-IDF 5.5.4 and FreeRTOS. One firmware image holds everything, the web pages included.

This page is the overview. The rules that keep the code in this shape are in [`AGENTS.md`](../../AGENTS.md), each with the header that explains it.

## Layers

Every component belongs to a layer and may depend only on its own layer and the ones below it. `components/LAYERS.txt` assigns them and `tools/check_tree.py` enforces it in CI.

| Layer | Components | What it holds |
|---|---|---|
| util | `p3a_util`, `frame_trace` | Tasks, jobs, events, memory pressure, errors, the clock. No knowledge of the product |
| board | `p3a_board`, `sdio_bus`, `slave_ota` | The display, touch, buttons, the SDIO bus shared by Wi-Fi and the SD card, the ESP32-C6's firmware |
| storage | `p3a_storage`, `p3a_settings` | The SD card (paths, atomic writes, health, repair, USB hand-over) and the settings store |
| library | `p3a_library`, `p3a_fetch` | Artwork records and indices, the blob store, pin lists, eviction; every HTTP request the device makes |
| sources | `source_local`, `source_makapix`, `source_giphy`, `source_klipy`, `source_museum`, `source_pinned` | One component per place artworks come from |
| player | `animation_decoder`, `display_renderer`, `animation_player`, `play_scheduler` | Decoding, the display pipeline, playing a file, deciding what plays next |
| ui | `p3a_ui`, `p3a_usb` | Screens, notices, overlays, touch; the card as a USB drive |
| net | `p3a_net`, `p3a_update` | Wi-Fi, setup, the clock, the HTTP server with its API and pages; firmware updates |
| main | `main/` | `app_main`: the boot sequence, nothing else |

A lower layer never calls up. Where it must tell an upper one something, it emits an event (`p3a_event.h`) or calls a hook the upper layer registered (`p3a_ui_hooks.h`, a source's command handler).

## From a source to the panel

1. **A playset** is a list of channels, each with a source, a kind, an identifier and a weight (`playset.h`). One playset is active.
2. **A source** is a table of functions (`p3a_source_ops_t`). For a channel it can list the artworks (`refresh`, into an index of `p3a_record_t`) and say where an artwork's bytes are (`url`). It does not download, schedule or delete.
3. **The content task** (`sched_content.c`) does the rest for every source alike: it lists channels when they are due, keeps their indices on the card, downloads artworks one at a time into the **blob store**, waits after a refusal, and makes room when the card fills. A blob is stored once under the hash of its content, whatever plays it.
4. **The scheduler** (`play_scheduler`) picks the next channel and the next artwork in it, moves on when the dwell time is up, and keeps the history. It is the only thing that asks the player for a file.
5. **The animation player** decodes the file (`animation_decoder`: WebP, GIF, PNG and APNG, JPEG, BMP) and reports each request as ready, shown or failed. What must happen when an artwork is really on the panel (history, counting a view) hangs off "shown".
6. **The display renderer** presents frames: decoded frames are scaled to the 720x720 panel (nearest-neighbour or hardware bilinear, as the source says), overlays are drawn on top, and buffers are swapped on the panel's vertical sync. Its timing code is the result of the jitter work and is not changed without measuring again against `docs/jitter/REPORT.md`.

What is on the screen at any moment is decided by the UI's model (`ui_model.c`): screens and notices are put up and taken down, and the one seen follows by precedence. Nothing switches screens directly.

## Pure logic and thin hardware

Whatever decides behaviour is written as plain C with no IDF includes, beside a thin file that does the device's part:

| Decision | Pure file |
|---|---|
| Which channel and which artwork next; when to move on; what a failure leads to | `sched_core.c`, `sched_pick.c`, `sched_turn.c`, `sched_history.c` |
| When a channel is listed again and how long a refusal waits | `sched_refresh.c` |
| Which blobs go when the card fills | `p3a_evict.c` |
| What a service's answer means | `giphy_parse.c`, `klipy_parse.c`, `makapix_parse.c`, `museum_<id>.c` |
| What a gesture does on each screen; what is seen | `ui_route()`, `ui_model.c` |
| Which release to install; when to look; whether a new firmware has proved itself | `update_release.c`, `update_policy.c` |

These are compiled and run on a workstation (`tests/host`, Unity, also under the sanitizers on Linux in CI), on answers recorded from the real services and on damaged copies of them. The device tests (`tests/device`, pytest) then flash the dev unit and drive it over its serial console and its API.

## Tasks, jobs, memory

- **Tasks** are few, long-lived, and created only through `p3a_task_create()` with a class (render, touch, app, network, background) that sets priority, core and where the stack lives. The display pipeline has core 1 to itself.
- **Jobs** (`p3a_job_submit()`) are for work that runs once or is slow: an install, a pin, a card format. An HTTP handler that needs more than a few milliseconds starts a job and answers 202 with its number.
- **Memory** defaults to PSRAM. Internal RAM is scarce (the Wi-Fi link and the SD card need it) and is asked for explicitly. Optional work checks `p3a_mem_level()` before it starts.
- **Flash mapping**: every task but the display pipeline has its stack in PSRAM, and the IDF does not allow flash to be memory-mapped from such a stack. The few calls that do it run on `app_main`'s task at boot or through `p3a_internal_call()`.

## Storage

Flash, fixed for the life of 2.x (`partitions.csv`):

| Partition | Size | What |
|---|---|---|
| `nvs` | 64 KB | Settings, Wi-Fi credentials, the Makapix registration |
| `otadata` | 8 KB | Which app slot boots, and whether it is on trial |
| `slave_fw` | 2 MB | The ESP32-C6's firmware, flashed onto it at boot when it is stale |
| `coredump` | 256 KB | The last crash |
| `ota_0`, `ota_1` | 10 MB each | The running firmware and the one before |
| `spare` | 9.6 MB | Reserved, unformatted |

The SD card (FAT) holds all artwork, under one root folder the user may move (`sd_path`): `animations/` (the user's own files), `blobs/` (everything downloaded), `channels/` (indices), `playsets/`, `pins/`, `temporary/`. Writes are atomic (`fs_atomic`), binary files are framed with a schema number and a checksum (`p3a_frame`), and a file that fails to load is never deleted. The card is formatted only from the on-device flow a person confirms.

Settings are one JSON schema (`components/p3a_settings/settings.json`): the build generates the C table and struct from it, the store validates against it, the API serves it, and the settings page draws itself from it.

## The network side

- `p3a_net_start()` returns at once; code that needs the network subscribes to events or asks `p3a_net_is_up()` and copes with "no". With no stored network the device opens the `p3a-setup` access point and serves the setup page.
- The HTTP server (`p3a_http.h`) is one route table per area and one envelope. Everything is under `/api/v2/` ([the API](api.md)). The pages are plain files under `components/p3a_net/static/`, gzipped into the firmware at build time; each page is a script, a model tested with node, and three shared scripts.
- Every request the device makes goes through `http_fetch`, which maps failures to a few errors callers act on (missing, rate limited, refused) and logs only a URL's host.
- Makapix Club: listings and reactions over HTTPS, commands and presence over MQTT with a per-device certificate.
- Firmware updates (`p3a_update`): the device looks at the repository's releases, installs `p3a-v2.bin` into the other app slot when its owner asks, and keeps a new firmware only once it is reachable again.
