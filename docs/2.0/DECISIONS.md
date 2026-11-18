# 2.0 decisions

Part of the [2.0 work stream](README.md). One entry per decision, with its
date and the reason. "Fab" means Fab decided it. "Working" means the agent
chose it to keep the work moving; a working decision stands unless Fab
objects, and it is listed so that he can.

A decision is changed by adding a new entry that names the one it replaces,
not by editing history.

## Fixed constraints (Fab, 2026-11-06)

Set before the architecture review and taken as given by everything since.

| ID | Decision |
|---|---|
| C1 | One board: the Waveshare ESP32-P4-WIFI6-Touch-LCD-4B |
| C2 | ESP-IDF 5.5.x. Not IDF 6 |
| C3 | C only |
| C4 | The web UI is vanilla HTML and JavaScript with no build step, served from the device |
| C5 | PICO-8 is out of 2.0 |
| C6 | Nothing carries over from 1.x: not SD contents, not NVS settings, not the Makapix identity or certificates. A 2.0 flash is a fresh device |
| C7 | No OTA from 1.x to 2.0. USB flash only |
| C8 | The four announced changes: A single binary with the web UI embedded, B 192-byte cache records, C repetition suppression (RAM only, reset by reboot), D aspect-ratio filter |

## Decisions

| ID | Date | By | Decision | Reason |
|---|---|---|---|---|
| D1 | 2026-11-17 | Fab | The partition table in `../2.0-architecture-review/target-architecture.md` section 4: no LittleFS, 10 MB app slots, a 9.6 MB unformatted `spare` | 2.0 is a serial reflash anyway, so the table is reordered once and then fixed for 2.x |
| D2 | 2026-11-17 | Fab | 2.0 is built on a long-lived `v2` branch in `fabkury/p3a`, pushed as work lands. `main` stays 1.x. `v2` replaces `main` when 2.0 ships | GitHub is the off-machine backup and CI can run on every push. The branch being public while unfinished is accepted |
| D3 | 2026-11-17 | Fab | Fresh skeleton: an empty layered tree, with 1.x modules moved in one at a time, bottom-up, each stripped of upward includes and landing with tests. A walking skeleton comes first so the device shows something early | The review rates an in-place refactor high cost because the dependency cycles are load-bearing, and a layered assembly low cost |
| D4 | 2026-11-17 | Fab | Scope is the whole review: 25 verdicts, A to D, the three ideas (clock overlay, Giphy and Klipy branding, VEXED pixel fonts), full 1.x parity minus PICO-8. New ideas go to `ideas.md` and are triaged, never added silently | One fixed baseline to plan and measure against |
| D5 | 2026-11-17 | Fab | The rule that forbade agents to build, flash or monitor unasked is removed for good, on `main` as well. Agents build, flash and monitor whenever it helps. The dev unit is owned by the development work; its flash, NVS and SD card may be erased | The old rule no longer reflects how the work is done, and 2.0 depends on agent-run testing |
| D6 | 2026-11-17 | Fab | Signed commits at each checkpoint, then push `v2`. If the signing prompt goes unanswered the work stays uncommitted, the ledger says so, and the session carries on | Keeps history signed without making the work wait on a popup |
| D7 | 2026-11-17 | Fab | Test-rig secrets live in a file under the user profile, outside the repo and outside Dropbox. A dev-only provisioning path feeds a freshly erased unit | Automated device tests must not stall on the captive portal, and the repo folder syncs to Dropbox |
| D8 | 2026-11-17 | Fab | The 32 GB card in the dev unit is healthy. SD errors during 2.0 testing are treated as firmware bugs | The earlier faulty 64 GB card is gone |
| D9 | 2026-11-17 | Fab | 1.x takes critical fixes only. One more release, 1.2.5, carries what is on `main` plus the lock-order fix (review finding 25); Fab cuts it when convenient. No new 1.x features | Keeps the 1.x fleet safe without splitting effort |
| D10 | 2026-11-17 | Fab | Manual checks are batched. When a milestone's automated tests pass, work moves on; checks only Fab can do accumulate in the ledger's manual checklist, each tied to its milestone, and a failed one reopens that milestone | Fab's time should not gate every milestone |
| D11 | 2026-11-17 | Fab | The web UI keeps its pages, layout and theme. Only the internals change: one `api.js`, scripts split out of the HTML, a schema-driven settings page, `/api/v2` paths, PICO-8 and web-UI-update pages removed | A visual redesign is a separate project |
| D12 | 2026-11-17 | Fab | 2.x releases ship the firmware under a new asset name and without `storage.bin`, in the same repository. 1.2.5 may carry a notice that 2.0 needs a USB flash | A 1.x unit takes the newest release that has an asset named `p3a.bin` and compares only version numbers, so a v2.0.0 with that asset would be offered to the whole 1.x fleet and land 2.0 on a 1.x partition table. With a new name 1.x finds nothing to install |
| D13 | 2026-11-17 | Fab | Makapix is tested against production Makapix Club. When that work starts, the dev unit registers as a new player and Fab does the one-time code entry on the site | There is no separate environment to maintain |
| D14 | 2026-11-17 | Working | Work-stream documents live in `docs/2.0/` (this folder); the review stays where it is as the design | Progress and design change at different speeds |
| D15 | 2026-11-17 | Working | At M0.1 the whole 1.x source tree moves to `legacy/` on `v2` and is never compiled again. Files leave `legacy/` only by being moved into the new tree or deleted with a reason. An empty `legacy/` is a release gate | The folder is a visible measure of what remains, and a module cannot be forgotten without leaving a file behind |
| D16 | 2026-11-17 | Working | Wi-Fi and a minimal `/api/v2` HTTP surface come up in Phase 1, before the player, although `net` is the top layer. Routes are added as their features land; the web UI itself still comes last | Every later milestone can then be driven and inspected over HTTP by automated tests. Layering is unaffected: `net` may call everything below it, and nothing below calls `net` |
| D17 | 2026-11-17 | Working | On-device UI (Phase 3) comes before the remote sources (Phase 4) | The sources need the channel-message surface, and Makapix registration needs the code screen. Touch control also makes the unit usable for Fab's manual checks sooner |
| D18 | 2026-11-17 | Working | Version string is `2.0.0-dev` until the release commit. One version number; `WEBUI_VERSION` and `P3A_API_VERSION` are not carried over | Change A removes the reason for the other two |
| D19 | 2026-11-17 | Working | Test tooling: Unity with CMake and CTest for host tests, pytest for device tests, `node --test` for web UI logic, GitHub Actions for CI. Details in [TESTING.md](TESTING.md) | All are already installed or dependency-free |
| D20 | 2026-11-17 | Working | A fresh unit is provisioned for tests over UART by a console that exists only in test builds. Credentials are never compiled into a binary | Build output sits inside the Dropbox-synced repo folder (D7) |
| D21 | 2026-11-17 | Working | The build configuration is `sdkconfig.defaults`; `sdkconfig` is generated and untracked. Options for components not yet in the tree are parked in that file, commented, under their milestone. Closes O11 | A tracked full `sdkconfig` would silently drop the esp_hosted and BSP settings while those components are absent, and bring them back as defaults later |
| D22 | 2026-11-17 | Working | What the event bus is for: fan-out only, when several parts that know nothing of each other must hear that something happened. It is not a way to call one known function; that is a direct call downward or the hook table upward. Emitting never blocks, handlers run later on the bus task with no lock held, and a full queue drops and counts. Categories are gone, and the 1.x event types are not carried over: each is added by the milestone that brings its emitter. The rule is at the top of `p3a_event.h`. Closes O8 | The review found 14 of 21 subscriptions were a relay through `main` for calls the emitter could have made |
| D23 | 2026-11-17 | Working | `sd_idle_wait` learns whether the IDF carries Espressif's own SD busy-wait fix (esp-idf #19034) from its CMake file, which reads the IDF source at configure time. 1.x asked the linker through a weak reference | The zero-weak-symbols rule has no exceptions, and a configure-time answer cannot silently become a no-op |
| D24 | 2026-11-17 | Working | `tools/compare_sdkconfig.py` guards the configuration for the rest of the migration: wherever the shipped 1.x `sdkconfig` and the generated 2.0 one both define a setting, they must agree, apart from a listed set of differences with reasons. A listed difference that disappears also fails | D21 replaced a 2,500-line tracked file with 60 lines of defaults; this proves nothing was lost and keeps proving it as components return |
| D25 | 2026-11-18 | Working | One container for every binary file on the card (`p3a_frame`): a 32-byte header (magic, schema, version, record size, count, flags, a checksum of the records and one of the header) followed by fixed-size records. A file that fails its check is never deleted or overwritten by the loader, which reports whether it is damaged, another firmware's, or absent. A file flagged rebuildable is rebuilt by its owner; a file flagged user data is left for recovery. Records carry their own state (availability is a flag in the record), so there are no extra sections | Review finding 5 (nine formats, nine policies) and the inherited rule that a failed load never deletes |
| D26 | 2026-11-18 | Working | A symbol that a dependency rule in some newer release of a pinned component asks about is defined in the pinning component's Kconfig, with the value that makes the rule false, and a comment saying when to remove it. Dependency changes get a from-scratch configure in the CI container before they are pushed | CI was red for a day on exactly this, and a new workstation would have failed the same way |
| D27 | 2026-11-18 | Fab | Setting keys are renamed to one convention: source prefix first, plain words, unit suffix last (`museum_refresh_s`, `giphy_refresh_s`, `makapix_refresh_s`, `dwell_s`, `indicator_size`). The 1.x name of each is kept in the schema's `was` field. Durations a user sets are in seconds, so `dwell_time_ms` became `dwell_s` | Nothing carries over from 1.x and the API becomes `/api/v2`; this is the one cheap moment |
| D28 | 2026-11-18 | Fab | Brightness is a stored setting, restored at boot (closes Q1). A factory reset exists, in the API and in the web UI behind a confirmation (closes Q5): it erases the whole NVS partition (settings, Wi-Fi credentials, the Makapix registration, recovery counters), leaves the SD card alone, and ends in a restart into setup | 1.x saved brightness and never read it; a user had no way back to a clean device |
| D29 | 2026-11-18 | Fab | The five settings that 1.x exposed only through the API (Giphy rendition, format and prefer-downsized; the Makapix view-acknowledge flag; the processing-indicator switch) stay settings, marked `advanced`: changeable through the API, not drawn on the settings page (closes Q7) | Same reach as today, no new UI to explain |
| D30 | 2026-11-18 | Working | Limits come from the schema and nowhere else. The dwell limit is 86,400 s (closes Q10). API keys are settings flagged `secret`: never logged, read back as a fixed mask, and the mask written back means "unchanged". Whether any call returns a key in clear is Q2, at M1.5 | 1.x had two dwell limits and returned keys in clear |
| D31 | 2026-11-18 | Working | Settings are stored as one NVS blob holding JSON of the values that differ from the defaults, keyed by name. A firmware that no longer accepts a stored entry skips it and keeps the rest; a blob that cannot be parsed is copied to a second key and the defaults are used. No struct layout or enum index is ever stored | A new default reaches every user who did not choose otherwise, and adding, removing or reordering settings needs no migration code |
| D32 | 2026-11-18 | Working | Where a value lives: Kconfig for the shape of the build; `settings.json` for what a user may change; `p3a_limits.h` for constants nobody changes; a component's own storage for device state (a Giphy random id, a registration, a counter). A default API key is never a build option | Review finding 21: 1.x spread these over Kconfig defaults, a JSON blob and ad hoc NVS keys |
| D33 | 2026-11-18 | Fab | Reading settings over HTTP always returns API keys masked. One separate, explicit call returns only the Harvard and Smithsonian keys, for the web UI's museum browse page, which calls those museums from the browser. The Giphy and Klipy keys never leave the device (closes Q2) | 1.x handed every key to anyone who read the settings |
| D34 | 2026-11-18 | Fab | Core dumps can be fetched over the network: the status call says whether one exists and gives its crash summary, one call downloads the raw dump, another erases it. The web UI says, next to the button, that a dump holds device memory and so may include the Wi-Fi password and API keys (closes Q6) | A crash on a user's device left no evidence anyone could reach without a cable |

## 1.x decisions that stay in force

Fab made these on closed 1.x work. They carry into 2.0 as product behaviour
and are not to be re-proposed or "optimized" away without a new reason.

Storage:

- Never delete a playset or store file because it failed to load.
- The `sd_health` failure latch is sticky until reboot. USB-MSC unexport does
  not clear it. Running out of space never counts as a failure.
- No auto-format on mount failure. FAT32 only; other cards get the "No Usable
  SD Card" screen with the explicit two-step on-device format. exFAT stays
  deferred (`docs/deferred/exfat.md`).
- A full card is handled by a 10-minute download backoff, not a UI notice.
- Eviction uses one continuous watermark formula with a 1 GiB guard floor,
  is layout-unaware, and never touches the user's own files.
- Every SD path resolves through `sd_path` with a runtime root. `/sdcard/p3a`
  is only the default.
- SD writes go through `fs_atomic`. Buffers for SD I/O are 128-byte aligned
  and multi-sector (the jitter fixes).
- A USB mass-storage session always ends in a clean reboot.

Playback and content:

- GIF and WebP loop counts are ignored. APNG `num_plays` is honored.
- Giphy `.404` markers have no TTL and no sweep.
- Refresh-error overlays have a 20-second lifetime and a cap of two per boot.
  The Giphy 401 notice stays until fixed.
- The aspect-ratio filter as designed in `docs/deferred/aspect-ratio-filter.md`:
  museum channels only, pinned and "show this" exempt, fail open, `ratio >
  max` rejects, range 2.5 to 10.0 in tenths, default 4.0, on by default, no
  download gating.
- Scheduler pick and channel-selection log lines stay at INFO. They are
  operator-facing.
- User-initiated swaps fail loudly; automatic swaps fail silently with
  bounded retries.

System:

- NVS stays 64 KB.
- Nothing installs unattended. The device flags an update; the owner
  approves it.
- No "renew certificate" button for Makapix. Renewal is automatic.
- The LAN API has no authentication.
- esp_hosted stays pinned at `~2.9.3` until the internal-RAM question in
  `docs/known-issues.md` is settled.
- Release builds carry no diagnostic overhead; trace and diagnostic options
  live in separate sdkconfig overlays.
- The silicon-revision guard and the `ESP_IDF_VERSION=5.5` rule in
  `AGENTS.md`.

## Open decisions

Each has the milestone before which it must be made. Fab decides unless
marked otherwise. They are repeated in the ledger's open questions so they
are not missed.

| ID | Decision | Due before | Notes |
|---|---|---|---|
| O1 | Exact asset name for 2.x firmware (D12), and whether the 2.x OTA check gets a compatibility field so that a future break cannot repeat the 1.x trap | M5.2 | Proposal: `p3a-v2.bin`, plus a `series` number in the release manifest that the device must match |
| O2 | Whether 1.2.5 carries an on-device or web UI notice about 2.0, and its wording | 1.2.5 | Independent of the 2.0 build |
| O3 | Text rendering: keep uGFX with converted fonts, or replace it with a small glyph-sheet renderer with integer scaling | M3.2 | See idea 3. The answer decides whether 54k lines of uGFX stay |
| O4 | Which VEXED families and sizes for large status text, body text and small labels; whether the clock shares them. Licence check of the chosen families | M3.2 | Fab picks; the agent can render candidates on the device for comparison |
| O5 | Clock overlay: 12 or 24 hour, seconds or not, visible during channel messages and the info screen or not | M3.5 | See idea 1 |
| O6 | Branding overlay: minimum band height and clear space from the Giphy and Klipy terms; bottom band only, or side bands too | M3.5 | See idea 2 |
| O7 | The screen model: `p3a_state` becomes the single owner, or it is deleted in favour of one UI model plus a small playback struct | M3.1 | Working decision allowed; the review offers both |
| O9 | The internal-RAM measures from `docs/known-issues.md`: packet mode, refresh concurrency, the Option E items, load shedding thresholds | M4.1 | Needs measurements on the 2.0 build, then a soak |
| O10 | A wall-clock budget for slow software JPEG decodes, and what a timed-out image becomes | M2.4 | The `NOT_PLAYABLE` error gives it a clean home |
| O12 | OTA hardening from audit item M23: post-boot self-test before marking an image valid, anti-rollback, minimum-version gate | M5.2 | |
| O13 | How `v2` becomes `main` at release: merge, or replace | M6.4 | |
| O14 | A second unit for the end-to-end field test (web flasher on a fresh board) | M6.3 | Needs hardware from Fab |
| O16 | Where a firmware install runs. `esp_ota_end()` verifies the image by memory-mapping flash, which a task with a PSRAM stack cannot do (see the 2026-11-18 ledger entry), and both job runners have PSRAM stacks. Options: a third runner with an internal stack for flash-mapping work, or verification on a dedicated internal-stack task | M5.2 | 1.x installed from the HTTP server's task, whose stack is internal |
| O15 | Whether Makapix needs any server-side change for 2.0 devices | M4.5 | Ask the Makapix side when Phase 4 starts |
