# Storage and data formats

Part of the [2.0 architecture review](README.md). Covers the SD card layout,
every on-disk format, the NVS settings store, atomicity and health, eviction,
and the identifier limits. Since a 2.0 flash is a fresh device and the SD card
may be re-laid-out, everything here is open.

Line numbers refer to `main` at `1692d43a`.

## 1. What is on the card

Every path resolves through `sd_path` (default root `/sdcard/p3a`), which is
the right discipline and should stay. What the firmware writes:

| Path under root | Owner | Format |
|---|---|---|
| `animations/*` | user, `/upload`, `show_url` | user files |
| `vault/{d0}/{d1}/{uuid}.{ext}` and `.404` markers | `download_manager` | Makapix artwork bytes |
| `giphy/{d0}/{d1}/{id}.{ext}` and `.404` | `giphy` | artwork bytes |
| `klipy/{gif,sticker}/{d0}/{d1}/{id}.{ext}` and `.404` | `klipy` | artwork bytes |
| `museum/{museum_id}/{d0}/{d1}/{iiif_key}.{ext}` | `art_institution` | artwork bytes |
| `pinned/lists/{slug}/{makapix,giphy,klipy/...,museum}/...` | `pin_lists_copy` | **duplicate** artwork bytes |
| `channel/{channel_id}.cache` | `channel_cache` | binary, header + 64 B records + LAi ints |
| `channel/{channel_id}.json` | `channel_metadata` | JSON `{cursor, last_refresh}` with `.bak` |
| `channel/{channel_id}.settings.json` | `channel_settings` | JSON, read-only, no writer exists |
| `channel/{channel_id}.bin` | legacy Makapix index | dead format; eviction still unlinks it (`storage_eviction.c:508`) |
| `channel/sdcard.bin` | `play_scheduler_cache` | 160 B records, **no header, no magic, no CRC** |
| `channel/ps_{hash}.playset` | `playset_store` | binary, 64 B header + 144 B packed entries |
| `active_playset.bin` (at root) | `active_playset_store` | 64 B header + **raw in-memory struct dump** |
| `playlists/{post_id}.json` | `playlist_manager` | pretty-printed JSON, raw `fopen "w"`, no fsync |
| `pinned/state.bin`, `pinned/lists/{slug}/manifest.json`, `order.bin`, `entries/*.bin` | `pin_lists` | four more formats |
| `temporary/*.tmp`, `*.tmp` and `*.bak` twins anywhere | `fs_atomic`, uploads | staging |
| `/sdcard/lost/` | `sd_repair` | quarantined directories |

Five parallel blob stores (vault, giphy, klipy, museum, pinned) share one
shard function (`sd_path_build_sharded`) but have separate path builders
(`download_manager.c:199`, `giphy_cache.c:54`, `klipy_cache.c:56`,
`art_institution.c:246`, `pin_lists.c:756`), separate `.404` handling and
separate exists-checks. Nine index and state formats, each with its own
magic, version and CRC policy, or none. `docs/infrastructure/configuration-and-development.md`
omits `pinned/`, `active_playset.bin`, `.playset` files and `lost/`.

## 2. The formats

| Format | Struct | Magic | Version | CRC | Discipline |
|---|---|---|---|---|---|
| `.cache` | `channel_cache.h:121-133` + `makapix_channel_impl.h:48-67` | `P3AC` | 25 | whole file | packed, LE, `_Static_assert`; mismatch = rebuild |
| `sdcard.bin` | `play_scheduler_types.h:121-133` | none | none | none | validity is `size % 160 == 0` (`playsets.c:181`) |
| `active_playset.bin` | `active_playset_store.c:56-67` | `0x50415033` | 1 | header + body | **unpacked `ps_channel_spec_t` dumped verbatim**, guarded only by a `spec_size` field; the comment at `:52-54` says cross-compiler portability "is not a goal" |
| `.playset` | `playset_store.h:48-83` | `P3PS` | 11 | yes | packed, defined serialization; `offset` reused reserved bytes without a bump (`:64-69`) |
| `pinned/state.bin` | `pin_lists_types.h:77-85` | `PSTA` | 2 | partial | packed |
| `order.bin` | `:91-138` | `PORD` | 2 | entries | packed union |
| `entries/*.bin` | `:144-159`, 512 B | `PENT` | 2 | **none** | packed |
| config blob | JSON in NVS `appcfg/cfg` | | none | | text, additive keys |

The `.cache` version has been bumped seven times (1, 20, 21, 22, 23, 24, 25),
each bump discarding every cache on every device and forcing a full
re-listing. The comment still says "File format v20" and "currently 1"
(`docs/known-issues.md:203` already notes it).

`ps_channel_spec_t` (`play_scheduler_types.h:160-191`) is about 860 bytes
because it carries a 716-byte `artwork` sub-struct that only the single
"show this" slot ever uses; the RAM half of that waste was already found and
half-fixed (`ps_artwork_state_t`, `:258-266`, "wasted ~36.8 KB of internal
RAM, 63/64 copies permanently zeroed"), but the spec still carries it. So
`ps_playset_t` is about 55 KB, `active_playset.bin` is 64 + 860 × N bytes
written on every playset execution, and each of the seven plain
`calloc(1, sizeof(ps_playset_t))` in `http_api_*.c` (`http_api.c:430`,
`http_api_rest_playsets.c:444,851,976`, `http_api_rest_status.c:229`,
`http_api_rest_actions.c:113`) zeroes 55 KB of PSRAM per request (plain
`malloc` above 8 KB goes to PSRAM under `CONFIG_SPIRAM_USE_MALLOC`).

**Fresh design.** One framed-record container: `{magic, schema_id, version,
record_size, count, flags, crc32}` followed by packed records, with one
loader that verifies, allocates in PSRAM and hands back a typed view. Every
index (channel caches, the SD scan, playsets, the active snapshot, pin order)
is an instance of it. "Version mismatch means rebuild" becomes one decision
in one place, and `flags` carries the disposability contract (section 7).
Nothing dumps an unpacked C struct to disk; the 144-byte packed playset entry
is the snapshot too.

**Migration cost.** Low. The packed formats already exist for most of these;
the work is one shared header and loader and deleting seven private ones.

**Verdict.** Would not build it this way. Fix in 2.0.

## 3. The 64-byte record and what 192 bytes should buy

Five record layouts share offsets 0 to 5 (`post_id`, `kind`, `extension`)
and a timestamp at offset 8 by convention; the discriminator is not in the
record (see the content-pipeline page for the casting). Identifier capacity
and its consequences:

| Source | Identifier field | Fit problems |
|---|---|---|
| Makapix | `storage_key_uuid[16]` | none; 28 bytes reserved |
| Giphy | `giphy_id[24]` | ids over 23 chars are skipped (`giphy_api.c:254`); `reserved[0]` repurposed as a rendition flag |
| Klipy | `klipy_id` u64 + `product` | CDN URL not reconstructible, re-resolved at download; 41 bytes reserved |
| Museum | `iiif_key[48]` | every adapter skips long keys (`artic.c:206`, `cma.c:250`, `ham.c:224`, `rijksmuseum.c:286`, `smithsonian.c:282`, `vam.c:190`); `extension` doubles as state (0xFF unresolved, 0xFE tombstone); two fail counters in bytes 62 and 63; Library of Congress deferred for this reason (`docs/deferred/library-of-congress.md`) |
| Pinned | union of the above; Klipy id stored as a 24-char string | museum branch keeps only the key and a museum id |

The `post_id` for non-Makapix sources is a salted 32-bit DJB2 of the string
identifier and is the key of the Ci and LAi hash tables; collisions within a
4096-entry channel are rare and unhandled, and `pin_lists` must re-derive the
same hash to route reactions.

**RAM budget at 64 versus 192 bytes.** `channel_cache_size` default 2048,
hard cap 4096, `PS_MAX_CHANNELS` 64, arrays in PSRAM:

| | per channel at 2048 | 64 channels | one 4096-entry channel |
|---|---|---|---|
| 64 B records | 128 KiB | 8 MiB | 256 KiB |
| 192 B records | 384 KiB | 24 MiB | 768 KiB |

24 MiB does not coexist with the 4.7 MB of frame buffers in 32 MB of PSRAM.
But the entry array is not the dominant cost today. Each `ci_post_id_node_t`
and `lai_post_id_node_t` (`channel_cache.h:48-60`, about 40 bytes with the
uthash handle) is allocated individually through `psram_malloc`
(`channel_cache_ops.c:50,111,263`), which is `heap_caps_aligned_alloc` at 128
bytes (`psram_alloc.h:27-35`, aligned for SD DMA). A full 4096-entry channel
therefore spends on the order of 512 KiB on hash nodes, about four times its
entry array. Load and save also stage the whole file in a second PSRAM buffer
(`channel_cache.c:169,623`).

**Fresh design for item B.** A 192-byte record with a `source` byte, a
common header (`id32`, `sort_at`, `modified_at`, `flags`, `width`, `height`,
`fail_counts`, `extension`), and a tagged union for identity sized for the
longest real identifier (a 96-character IIIF key, a UUID, a u64 plus
product, a Giphy id). Replace uthash with an open-addressed `uint16_t` slot
table stored beside the array (8 KiB at 4096 entries) and a `uint8_t`
availability bitmap for LAi (512 bytes). The 192-byte channel then costs less
PSRAM than the 64-byte channel does today. Cap resident caches at 16
concurrent channels rather than 64; a playset with more channels loads them
on demand.

`width` and `height` in the common header are what the aspect-ratio filter
(item D) needs, filled by the loader on first decode exactly as
`docs/deferred/aspect-ratio-filter.md` describes, but as a plain field instead
of six per-source side channels.

**Verdict.** 192 bytes is the right call and is cheaper than it looks once
the hash nodes go.

## 4. NVS and the settings store

| Namespace | Keys | Through `config_store`? |
|---|---|---|
| `appcfg` | `cfg`, `cfg_new` (one JSON blob, max 8 KB) | yes |
| `appcfg` | `wifi_rst_*`, `touch_rst_*`, `tp_rst_*` u16 counters | same namespace, outside the blob (`config_store_giphy.c:378-396,441,483`) |
| `makapix` | player key, broker host and port, three PEMs, API token | no (`makapix_store.c:17-24`) |
| `wifi_config` | `ssid`, `password` | no |
| `sd_repair` | `total` | no |
| `ota` | `webui_invalid`, `webui_recover`, `webui_failures` | no; literals repeated in `p3a_board_fs.c:80,128` |
| `jtr` | debug blob | no |
| `p3a_boot`, `p3a_state` | listed for erase in `fresh_boot.c:146-151`; no writer exists | stale |

Brightness is not persisted at all (`app_lcd_p4.c:158-165`).

`config_store` is one JSON object written double-buffered (`cfg_new` then
`cfg`, `config_store.c:207-243`). Twenty-six keys are read; `config_store.h`
exposes 95 functions. Of 45 getters, about 20 cache their value after the
first read; the other 25 open NVS, read the blob and parse it with cJSON on
every call (`cfg_get_string` at `config_store.c:58-78` is the template). Every
setter loads, modifies, serializes, writes two blobs and commits twice. There
is no mutex (`docs/known-issues.md` M5).

Adding one setting touches `config_store.h`, one `config_store_*.c`,
`http_api_rest_settings.c` (a hand-written GET line and PUT branch),
`webui/settings.html` (107 element ids, no schema object), often
`http_api_rest_status.c`, and `WEBUI_VERSION`. Measured: `ai_refresh_sec`
appears in 12 files, `channel_cache_size` in 10. `PUT /config?merge=true`
merges any incoming key with no whitelist (`http_api_rest_settings.c:116-142`;
`config_store_save` checks only "is an object"), so a typo persists forever.
Defaults live in each getter, so `GET /config` returns only keys that were
ever written plus a few hand-appended ones. Ranges are enforced in C and
re-typed in JS, and already disagree (see the web UI page).

**Fresh design.** A settings schema as the single source of truth: one table
of `{key, type, default, min, max, reboot_required, group, label}` in a small
JSON file in the repo. The build generates the C table (typed struct, defaults,
validation) and the UI reads the same JSON at runtime to render the settings
page and validate before sending. The store keeps a parsed struct in RAM,
reads NVS once at boot, and writes a blob on change behind a mutex. The
stray counters, `ota` flags and `sd_repair` total join the schema; `p3a_boot`
and `p3a_state` leave the erase list. Wi-Fi credentials and Makapix secrets
stay in their own namespaces because they are secrets, not settings.

**Migration cost.** Medium, paid once; it is the kind of generator that costs
two days and saves an afternoon per setting forever.

**Verdict.** Would not build it this way. Fix in 2.0.

## 5. Atomicity and health

`fs_atomic_write_cb` (`fs_atomic.c:93-150`) does tmp, fsync, unlink the final
name (FATFS rename never overwrites), rename, optional `.bak` rotation,
reports to `sd_health`, and heals an overwritten directory through
`sd_repair`. This is the right primitive and the review found nothing to
change in it.

Writers that bypass it:

| Site | What | Risk |
|---|---|---|
| `playlist_manager.c:580` | `playlists/{id}.json`, `fopen "w"`, no fsync | torn JSON on power cut |
| `pin_lists_entry.c:36` | 512 B entry, direct write | not atomic, no CRC |
| `http_fetch.c:656`, `http_api_upload.c:124` | own tmp+rename | equivalent, duplicated |
| `download_manager.c:955`, `play_scheduler_cache.c:100,127,187,239` | `.404` markers, 0-byte sentinel, mtime touch | harmless |

FAT32 assumptions are spread through the code rather than stated: long
filenames required (`sd_repair.c:29-35` counts LFN slots for its probe),
`CONFIG_FATFS_API_ENCODING_ANSI_OEM` (non-ASCII names in `animations/` are a
gamble), 40 `strcasecmp` sites on `d_name`, rename-does-not-overwrite handled
in two places, 2-second mtime resolution relied on by
`makapix_cache_recover_and_cleanup` (`makapix_channel_utils.c:82`) and by
eviction, and `esp_vfs_fat_info("/sdcard")` hard-coded in
`storage_eviction.c:165`.

**Fresh design.** Keep FAT32 (exFAT stays deferred for the reasons in
`docs/deferred/exfat.md`). Route the two remaining tmp+rename copies through
`fs_atomic`. Make `playlist_manager` either atomic or gone (the content page
shows it is dead). State the FAT assumptions once in a header the storage
layer owns.

**Verdict.** Keep, with small fixes.

## 6. Eviction and identity: five blob stores, zero deduplication

Eviction (`storage_eviction.c`) is layout-unaware, ages by `st_mtime`, halves
its threshold from 30 days to a 4 hour floor, triggers at 10 percent free and
stops at plus 25 percent, and refuses to run without SNTP. The design is good.

What it walks is five shard trees with identical hashing and no shared
identity. Nothing is keyed by content. Pinning copies the bytes
(`pin_lists.c:859-861`, `pin_lists_copy.c:55-81`), so a pinned artwork lives
twice, and once more per additional list. Giphy and Klipy never dedupe
against each other or the vault. The download task discovers existence with
`stat()` per entry (`download_manager.c:813`) and `lai_verify` re-walks under
miss pressure.

**Fresh design.** One blob store: `blobs/{d0}/{d1}/{sha256-prefix}.{ext}`
plus a small `refs` record (which channels and lists reference the blob, and
whether any of them is user intent). The hash is computed while the bytes
stream through `http_fetch`. Then `.404` handling, exists-checks, eviction and
pinning are each one implementation; pinning is a reference bump and never
copies; and eviction can be told "never delete a blob with a pin ref" by data
rather than by path prefix. The availability set is the blob index, so LAi is
a bitmap over Ci and the `stat()` loops disappear.

**Migration cost.** Medium. It is new code, but it replaces five path
builders, four exists-checks and the copy path, and it is the natural home
for the never-delete rule that is today a convention.

**Verdict.** Would not build it this way. Fix in 2.0.

## 7. Playsets: three representations and a 32-character wall

A playset exists as JSON over the wire (`playset_json.c`), 144-byte packed
entries on disk (`playset_store.h:70-83`) and the 860-byte in-memory struct
dumped into `active_playset.bin`. The `identifier[33]` field has forced two
shipped workarounds: CMA stores a 32-character truncation and expands it
through a hard-coded table (`cma.c:91-100`), and Wellcome hides every facet
label over 32 characters in the browser (`wellcome.c:17,159-160`,
`docs/deferred/wellcome-long-labels.md`). JSON parsing `strncpy`s and silently
truncates (`playset_json.c:110-141`). `storage_key` is 64 bytes in the spec,
96 in `ps_artwork_t`, 40 in `channel_cache_merge.c:51`. `channel_id` is 16
hex characters of SHA256 over `type:name:identifier:offset`, so any change to
an identifier re-homes the cache; the cache header stores 15 of the 16
characters (`channel_cache.c:638-642`).

**Fresh design.** One canonical playset record (the packed one), 96-byte
identifiers, one width per concept, and a `channel_id` computed over a
canonical form. Delete the `artwork` sub-struct from the channel spec; the
"show this one" case is a separate small struct, as `ps_artwork_state_t`
already half-is.

**Verdict.** Would not build it this way. Fix in 2.0, and delete the CMA and
Wellcome tables when it lands.

## 8. State the disposability contract in the data

`docs/known-issues.md` already asks for a stated contract on what is
rebuildable. Today eviction knows it may delete `channel/*.bin` and must not
touch `pinned/` only by path prefix. With one record container and one blob
store, a `flags` bit per index ("cache, rebuildable" versus "user data, back
up") and a `refs` entry per blob let eviction, `sd_repair`, fresh-boot and the
USB-MSC story make the same distinction from data. It also tells a future
"export my pins" feature exactly which files to copy.

## 9. What is good and should be kept

- `sd_path` as the one place paths are built, with a runtime root.
- `fs_atomic`, `sd_health` and `sd_repair`, unchanged.
- The two-level 6-bit shard with FNV-1a, and the layout-unaware eviction walk.
- The `.playset` packed format's discipline (magic, version, CRC, defined
  serialization) is the model every other format should follow.
- The double-buffered NVS blob write is correct; only what is inside it needs
  a schema.
- Secrets separated from settings in NVS.
