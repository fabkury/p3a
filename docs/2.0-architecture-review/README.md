# p3a 2.0: a fresh-eyes architecture review

**Question asked.** If p3a were rebuilt from scratch today, would it be
architected the way 1.x is? Where not, what would change, and what would it
cost to move?

**Scope.** Firmware internals, the content pipeline, storage and data
formats, the HTTP API and web UI. Fixed constraints: the Waveshare
ESP32-P4 4B board only, ESP-IDF 5.5.x, C, vanilla HTML/JS served from the
device. PICO-8 is out of 2.0. A 2.0 flash is a fresh device; nothing on the
card or in NVS carries over, and OTA from 1.x is not possible.

**Method.** Read the code on `main` at `1692d43a` (about 100k lines of
project C plus 7.5k lines of web UI) with the docs beside it. Every finding
cites file and line. Four claims that carry the most weight were re-verified
by hand (the lock-order inversion, the dead channel-switch path, the
per-request settings re-parse, the hash-node alignment cost). Nothing was
built, flashed or edited outside this folder.

**Register.** Candid. The repo is a year old, has 1,485 commits, ships to
real devices and has a measured, documented jitter fix that most hobby
firmware never gets. The structure below is what a year of fast feature work
on a moving target produces; the point of the review is that 2.0 is the one
moment the structure can be changed without breaking anyone.

## The thesis in one paragraph

The parts of p3a that talk to hardware or to servers are good and should
carry over: the display pipeline, the decoders, the jitter fixes,
`http_fetch`, the SD atomicity and repair primitives, the museum and Makapix
adapters, Wi-Fi and provisioning. The parts that connect those to each other
are where a fresh design diverges, and they diverge for one shared reason:
there is no source interface. Six content sources are dispatched by type
ladders repeated in a dozen files, their records are five layouts cast into
one, their bytes live in five parallel blob stores, and the components that
hold them require each other in cycles that the build papers over with
`WHOLE_ARCHIVE` and 97 weak symbols. Almost everything else in the review
follows from that: the per-source refresh and download copies, the
discriminator that is not in the file, the identifier limits that forced two
museum workarounds, the scheduler that must be linked before a cache file can
be read. A 2.0 that introduces one record type, one blob store and one
`source_ops_t` table, and layers the components so arrows point one way, gets
the four announced changes (A, B, C, D) as small additions instead of
six-place edits, and gets a host test harness as a side effect.

## How to read this folder

| Page | Covers |
|---|---|
| [firmware-internals.md](firmware-internals.md) | Tasks, memory, state machine, event bus, services, dependency graph, config tiers, errors, testability |
| [content-pipeline.md](content-pipeline.md) | Scheduler, channels, caches, download, loader; the per-source duplication; where C and D land |
| [storage-and-data.md](storage-and-data.md) | SD layout, the nine formats, the 64-byte record and what 192 buys, NVS and settings, atomicity, eviction |
| [web-ui-and-api.md](web-ui-and-api.md) | Routing, envelope, slow handlers, dual OTA and everything A deletes, embedding, the UI's structure |
| [target-architecture.md](target-architecture.md) | The proposed 2.0 skeleton with A, B, C, D placed in it |

Each finding on the area pages has the same shape: what 1.x does with
evidence, why it got there where that is fair to say, what a fresh design
does, what it costs to move, and a verdict.

## Verdict table

"Greenfield" is whether a from-scratch design would make the 1.x choice.
"Cost" is the effort to move 1.x code to the fresh design, given that 2.0 is
already a clean break. "2.0" is the recommendation.

| # | Finding | Greenfield | Cost | 2.0 |
|---|---|---|---|---|
| 1 | Components form cycles; `main` is a component in disguise; 97 weak externs hide dependency edges | No | High as a refactor, low if 2.0 is assembled layered from the start | Do first; everything else gets cheaper |
| 2 | Six sources, no source interface; type ladders in a dozen files; three copies of merge and evict, nine vault-path builders | No | Medium | Do; prerequisite for B, C, D being small |
| 3 | Five record layouts cast into one; discriminator not in the file | No | Medium | Do; required by B anyway |
| 4 | Five parallel blob stores; pinning copies bytes; availability by `stat()` | No | Medium | Do; one blob store keyed by content hash |
| 5 | Nine index formats with nine version policies; `active_playset.bin` dumps an unpacked struct; `sdcard.bin` has no header | No | Low | Do; one framed container |
| 6 | Settings: JSON blob re-parsed on 25 of 45 reads, no schema, no PUT whitelist, ranges typed twice | No | Medium, once | Do; schema as single source of truth |
| 7 | Loader commits history before the bytes are read; loader and render coordinate through shared flags and timeouts | No | Medium | Do; `swap_shown` is what C needs |
| 8 | Makapix refresh is five layers deep (per-channel task, pending table, poll loop) | No | Medium | Do; run it on the refresh task |
| 9 | 42 tasks, two stack policies, three priority conventions, twelve one-shot tasks, no job runner | No | Low | Do |
| 10 | Four allocation idioms; no memory-pressure signal despite an open production crash | No | Low, plus a soak | Do; the one item that also closes a known issue |
| 11 | State machine is one of seven copies of "what is on screen" | No | Medium | Do with 1 |
| 12 | Event bus is a one-hop relay into `main` | No | Low | Decide what it is for, then enforce |
| 13 | Three forwarding services in `main/` | No | Trivial | Delete |
| 14 | No project error base; failures classified by path substrings | No | Low | Do; D needs it |
| 15 | Web UI in its own partition with its own OTA, version, health check and recovery page | No | Low | Change A is correct; about 2,300 lines go |
| 16 | Routing by `strcmp` chain; three settings surfaces; five activation verbs; nine dead endpoints | No | Low | Do; `/api/v2/` |
| 17 | Response envelope by convention; 14 handlers skip `data` | No | Low | One `send_ok` |
| 18 | Firmware OTA install and FAT free-space walk run on the httpd task | No | Low once 9 exists | Jobs |
| 19 | UI: no shared request helper (70 call sites), limits typed in C and JS and already disagreeing, museum tables duplicated in JS and C | No | Low to medium | `api.js`, limits from the schema, `museums.json` |
| 20 | 32-character identifiers forced CMA and Wellcome workarounds and deferred Library of Congress | No | Low with B | 96 bytes |
| 21 | Eight dead Kconfig options, fifteen inherited Wi-Fi entries, no stated Kconfig/NVS/constant rule | No | Trivial | Delete and write the rule |
| 22 | No host tests, no build-on-push; logic files include `p3a_state.h` | No | Medium, once | Do early; the only item that costs more later |
| 23 | About 3,000 lines of dead code (`content_cache`, `playback_queue`, the vtable, `sdcard_channel_impl`, `makapix_channel_switch`, `playlist_manager`, legacy setters) | n/a | Trivial | Delete before touching anything else |
| 24 | Image assets as 390 KB of hex C arrays | No | Trivial | `EMBED_FILES` |
| 25 | Lock-order inversion in the three `evict_orphans` paths against `execute_playset` | n/a | Trivial | Fix in 1.x now; unobserved on hardware but real |

Things the review looked at and would build the same way: the display
pipeline, the decoders, SWRR with credit rollback, the loud/silent swap
failure split, `http_fetch` with its TLS gate, `fs_atomic` and `sd_repair`,
`sd_path` with a runtime root, the shard scheme, layout-unaware eviction,
`frame_trace` and `mem_stats` compiling to nothing, the browser-side museum
browse with device-side refresh, the `.playset` format's discipline, the
`ETag`/304 on the active playset, and the decision not to add auth to a
LAN-only API.

## The four announced changes, judged

- **A, single binary.** Correct, and the review found nothing in favour of
  the split. The embedded UI is about 150 KB gzipped once the PWA icons are
  shrunk, in an 8 MB slot that is 2.46 MB full. Details and the deletion list
  on the web UI page.
- **B, 192-byte records.** Correct, and cheaper than it looks: today's
  per-node hash allocations at 128-byte alignment cost about four times the
  entry array, so a 192-byte record with an inline slot table uses less
  PSRAM than a 64-byte record does now. Cap resident channel indices at 16.
  Details on the storage page.
- **C, suppress repetitions.** Small once there is one pick function and an
  honest `swap_shown` event; neither exists in 1.x. Details on the content
  page.
- **D, filter aspect ratio.** The deferred design is sound for 1.x and its
  four implementation traps are all structural consequences of 1.x; in the
  2.0 skeleton they do not exist. Details on the content page.

## Two things worth doing regardless of 2.0

1. Fix the lock-order inversion in `giphy_refresh.c:484-489`,
   `klipy_refresh.c:193` and `art_institution_refresh.c:233` (finding 25).
2. Delete the dead code in finding 23. It is roughly 3,000 lines and several
   of the dependency cycles run through it.

## What this review is not

It is not a plan with dates, not a line-by-line audit of the museum adapters
or the Wi-Fi manager (both judged sound at the structural level and not read
in depth), and not an opinion on product scope beyond the PICO-8 decision
already made. The target-architecture page sketches an order of work by
dependency, not by calendar.
