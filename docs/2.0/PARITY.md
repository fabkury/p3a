# 2.0 parity checklist

Part of the [2.0 work stream](README.md). 2.0 ships with everything 1.x
does, minus PICO-8 (decision D4). This page lists what "everything" is, so
that a rewrite cannot quietly lose a behaviour.

**Where the list comes from.** `README.md`, `docs/HOW-TO-USE.md`, and a sweep
of the 1.x code on `main` at `4c9de368` on 2026-11-17 (HTTP routes, MQTT
commands, settings keys, gestures, screens, scheduler rules, web UI
controls). The sweep was done by a read-only agent and has not been checked
line by line. Each row is re-checked against the code in `legacy/` when its
milestone moves it; correct the row if the code says otherwise.

**How to use it.** The "2.0" column says what happens to the capability:
`keep` (same behaviour), `change` (same purpose, new shape, described in the
note), `drop`, or `decide` (see the list at the end). "M" is the milestone
that delivers it. Tick the box when the milestone's tests cover the row.
M6.2 requires every row ticked or dropped.

HTTP paths are not parity items. The API moves to `/api/v2` with one
envelope and one way to do each thing (review findings 16 and 17), so rows
describe what a client can do, not the 1.x URL.

## Content sources

| | Capability | 2.0 | M | Note |
|---|---|---|---|---|
| [x] | Local files from `{root}/animations/` only | keep | M2.3 | |
| [x] | Upload over HTTP: 16 MiB cap, webp gif jpg jpeg png apng bmp, name reduced to its basename, overwrites, plays at once, refused while the card is shared over USB | keep | M2.3 | Done in M2.3 except: plays at once (M2.4) and the USB refusal (with USB sharing). A name the card cannot hold is refused, not reduced Plays at once since M2.4; refused while a computer has the card since M3.6 |
| [ ] | The home page shrinks oversized JPEGs in the browser before upload | keep | M5.1 | |
| [ ] | Makapix channels: all, promoted, by user, by hashtag, a user's reactions, single artwork | keep | M4.5 | |
| [ ] | Promoted refreshes over public HTTPS on an unregistered device, so a new device plays art with no account | keep | M4.5 | First-run experience depends on it |
| [ ] | `followed_artists` and other playsets fetched from the Makapix server by name | keep | M4.5 | |
| [x] | Giphy: trending and search channels; rating; country code; refresh interval; cache size | keep | M4.2 | M4.2. Live device tests for trending, search, rating, cache size and interval; country code and offset are in the request (host-tested), not seen on the device |
| [x] | Giphy: rejected-key latch with hourly re-probe, 429 cooldown, analytics id with reset, click registered on swipe up | keep | M4.2 | The waits are the content task's for every source (D66): an hour or a settings change after a refused key, at least 15 minutes after a 429. The id, its reset and the click are device-tested; a 429 was not provoked |
| [ ] | Giphy: rendition, format and prefer-downsized, settable through the API only | decide | M4.2 | Q7 |
| [x] | Klipy: GIF or sticker, each as trending, search or category; own key; rating; format; refresh interval; cache size | keep | M4.2 | M4.2. Live device tests for GIF trending and sticker search in both formats; categories are a search by name (host-tested) |
| [ ] | Klipy: 36 fixed categories offered in the editor | keep | M5.1 | |
| [ ] | Museums: Rijks, V&A, Wellcome, SMK, Harvard, Smithsonian, Cleveland, Minneapolis; AIC present but marked unavailable, cached AIC art still plays | keep | M4.3 | |
| [ ] | Museums: Harvard and Smithsonian keys; a channel without its key sleeps and wakes when the key is saved | keep | M4.3 | |
| [ ] | Museums: Rijks resolved lazily in the background; refresh interval and cache size settings | keep | M4.3 | |
| [ ] | Museums: 429 cooldown shared between device and browser | keep | M4.3, M5.1 | |
| [ ] | Museums: long identifiers handled by truncation tables (CMA) and hidden terms (Wellcome) | change | M4.3 | 96-byte identifiers; the workarounds are deleted |
| [ ] | Pin lists: up to 64 lists of 4096; one active list; "My Pins" created and recreated automatically; the active list cannot be deleted | keep | M4.4 | |
| [ ] | Pin lists: create, rename, set active, delete, pin, unpin, is-pinned, page through, entry detail, fetch the artwork file, play as a channel | keep | M4.4 | |
| [ ] | Pinning copies the artwork bytes into the list's folder | change | M4.4 | A pin becomes a reference in the blob store |
| [ ] | Pinnable: Makapix, Giphy, Klipy, museum. Not local files | keep | M4.4 | |
| [ ] | Show a URL: http or https, 16 MiB, saved to `animations/`, with or without an on-screen progress bar; refused while the card is shared | keep | M4.4 | |
| [ ] | Swap to a named local file or a specific Makapix post | keep | M4.4 | Used by the Makapix site |

## Playback

| | Capability | 2.0 | M | Note |
|---|---|---|---|---|
| [ ] | Animated WebP, GIF, APNG; static PNG, JPEG, BMP; transparency | keep | M2.1 | Decoders moved and pixel-tested against a corpus (part 1). Two 1.x bugs fixed on the way: JPEG colour range (D38), animated WebP blending. Short frame delays now play as in browsers (D37). On screen with part 2 |
| [x] | Fit to screen with aspect ratio kept, centred, borders in the background colour; the colour also prepaints boot | keep | M2.4 | Done in M2.1 |
| [x] | Bilinear upscaling for Giphy, Klipy and museum art; nearest-neighbour for the rest | keep | M2.4 | The source's table says which (`smooth`); the user's files: O18 |
| [ ] | Pinned Giphy, Klipy and museum art plays nearest-neighbour, because the choice is made by channel type | decide | M4.4 | Q4 |
| [ ] | GIF and WebP loop counts ignored; APNG `num_plays` honored | keep | M2.1 | |
| [x] | Max-speed playback setting | keep | M2.4 | Done in M2.1 |
| [x] | Auto-advance every 30 s by default; 0 turns it off; the timer resets on manual navigation and on request | keep | M2.4 | |
| [ ] | Auto-advance holds while the info screen, the setup screen or pause is up, and while the loader is busy | keep | M2.4, M3.3 | Pause and a busy loader: done (M2.4). Screens hold it through `play_scheduler_hold()` from M3.3 |
| [ ] | Pick mode random or recency; channel selection SWRR or stochastic | keep | M2.3 | |
| [x] | Previous walks a history of 32 without disturbing the generator; history survives a playset switch | keep | M2.3 | |
| [x] | History and last-played commit when a swap is requested | change | M2.4 | They commit when the artwork is shown (finding 7) |
| [x] | Pause: black screen, backlight off, views and auto-advance stopped; resume restores brightness; not kept across reboot | keep | M2.4 | Views stop with the view tracker (M4.5) |
| [ ] | Rotation 0, 90, 180, 270: persisted, applies to playback, screens and overlays | keep | M2.4, M3.3 | Playback: done (M2.1) |
| [x] | User-requested swaps fail loudly; automatic ones retry silently (3 retries, a small blocklist, a skip budget) | keep | M2.4 | Changed (D55): "next" also tries other artworks and reports only if all fail; a named artwork reports at once. No blocklist: a failed artwork is set aside until its channel is listed again |
| [ ] | A corrupt cached file is deleted, at most one per minute; local files are never deleted | keep | M2.4 | Through the blob store, on `CORRUPT` only Wired in M2.4; seen on the device when a source makes blobs (M4.1) |
| [x] | Artworks the server no longer has are remembered and skipped | keep | M4.1 | One implementation: the record's `gone` flag, set by the content task on a 404. Device-tested with the test source |
| [ ] | A high miss rate triggers a verification sweep of what is on the card | change | M2.2 | Availability comes from the blob index; a miss is a repair event |
| [ ] | Boot restores the active playset; with none, plays Promoted without saving that choice | keep | M2.3, M4.5 | Restoring is done (M2.3). With none, 2.0 plays the user's own files (D53) until Makapix exists; revisit at M4.5 |
| [x] | First swap waits for the first available artwork, a finished refresh, or the timer | keep | M2.4 | |
| [x] | One-time "refresh every channel now" switch | keep | M4.1 | `POST /api/v2/player/refresh`. Device-tested |
| [ ] | Makapix channels other than Promoted do not refresh on an unregistered device, and the UI says so | keep | M4.5 | |
| [x] | Per-channel refresh failures back off | keep | M4.1 | `sched_refresh.c` (D66). Host-tested; the rate-limit and no-key waits seen on the device |

## Playsets

| | Capability | 2.0 | M | Note |
|---|---|---|---|---|
| [ ] | Up to 64 channels, each with a weight and an offset; all-zero weights mean equal shares | keep | M2.3 | At most 16 channel indices resident at once |
| [ ] | Offset honoured for museum, Giphy, local and pinned channels | keep | M2.3 | Review asks for one meaning per source, stated |
| [ ] | Built-in playsets: all Makapix, Promoted, SD card, Giphy trending | keep | M2.3 to M4.5 | |
| [x] | A library of up to 32 saved playsets: list, read, create, update, rename, delete | keep | M2.3 | Rename is save under the new name, then delete the old |
| [x] | Playset names of at most 32 characters; identifiers of at most 32 | change | M2.3 | Identifiers grow to 96 bytes |
| [ ] | Switching is asynchronous, newest request wins, and the result is reported with a sequence number and error code | keep | M2.3 | The web UI depends on this |
| [ ] | The active playset is readable cheaply (ETag, 304) with its banner flags | keep | M2.3 | Readable since M2.3 without the ETag; add it in M5.1 if the UI polls |
| [x] | A playset file that fails to load is never deleted | keep | M2.3 | |

## Touch and buttons

| | Capability | 2.0 | M | Note |
|---|---|---|---|---|
| [x] | Tap right or left half: next or previous, in rotated coordinates | keep | M3.4 | |
| [x] | A touch that drifts without becoming a swipe is discarded, not a tap | keep | M3.4 | |
| [ ] | Swipe up: pin; also Like on Makapix art, click on Giphy art. Swipe down undoes it. Error indicator on local files | keep | M3.4, M4.4, M4.5 | Recognised since M3.4; what it does arrives with the sources |
| [x] | Two-finger rotate past 45 degrees turns the screen 90 degrees, once per gesture | keep | M3.4 | |
| [ ] | Long press (2.25 s): info screen, or the setup screen in setup mode; dismisses any screen | keep | M3.4 | Done in M3.4 for the information screen and for closing; in setup mode the setup screen is up by itself in 2.0 |
| [x] | While paused, only long press works, and it resumes | keep | M3.4 | |
| [x] | While a screen is up, taps reach only the SD-format controls | keep | M3.4 | Taps are ignored under a screen (M3.4); the format controls come with that screen (M3.3) |
| [ ] | During registration, long press cancels and returns to playback | keep | M3.4 | The long press closes the screen (M3.4); cancelling the registration is M4.5 |
| [x] | During a firmware update, gestures are ignored | keep | M3.4 | |
| [ ] | BOOT button toggles pause; ignored during the co-processor update | keep | M1.2, M2.4 | Wired in M2.4; manual check M2.4-a |
| [ ] | Touch failure at boot: countdown and one reboot, then carry on without touch | keep | M3.4 | The bounded init is in M1.2; the reboot streak needs settings (M1.4) and the countdown needs the UI The countdown is on the screen since M3.3; not seen with a failing controller |

## On-screen UI

| | Capability | 2.0 | M | Note |
|---|---|---|---|---|
| [x] | Boot: background painted before the backlight, then one of 22 intro animations (random or chosen, 1 to 7.5 s) | keep | M3.3 | |
| [ ] | Info screen: name and version, uptime, Wi-Fi and signal, Makapix state, Giphy key state, card space, playset counts, web UI address, IP, reboot counters, format button when the card has failed | keep | M3.3 | Done in M3.3 except the Makapix and Giphy rows (with those sources) and the reboot counters |
| [ ] | Setup screen: network name, both addresses, connected clients; appears when setup mode starts | keep | M3.3 | |
| [ ] | Registration: status, the 6-character code with a colour-coded countdown, success notice for 10 s; expiry leaves the mode | keep | M3.3, M4.5 | |
| [ ] | Firmware update screen with versions and progress | keep | M3.3, M5.2 | |
| [ ] | Co-processor update screen with reboot countdown | keep | M1.2, M3.3 | |
| [ ] | Channel messages: fetching, downloading with progress, failed, empty, loading, waiting for network, empty pin list, no playable files, key needed, museum unavailable, starting | keep | M3.3 | One message surface (finding 11) |
| [x] | Refresh-error notices with a 20 s life and a cap of two per boot; the Giphy invalid-key notice stays | keep | M4.2 | D71. The refused-key notice is device-tested; the once-only ones are host-tested |
| [x] | "SD card is not working" and "SD card repaired" notices, 20 s, once per boot | keep | M3.3 | |
| [x] | "No Usable SD Card" screen and the two-step on-device format | keep | M3.3 | The device boots on behind the screen in 2.0 |
| [ ] | USB screens: card exposed, disconnected and restarting | keep | M3.6 | |
| [ ] | Recovery countdowns for the Wi-Fi chip and the SDIO link, and the offline notice | keep | M1.5 | |
| [ ] | "No Internet Connection" screen | drop | | Never shown in 1.x |
| [x] | Overlays: FPS, processing indicator (size setting), reaction icons, pin icons, all following rotation | keep | M3.5 | Done in M3.5; the icons' real triggers come with the sources |
| [ ] | All text in DejaVu Sans through uGFX | change | M3.2 | VEXED pixel fonts (idea 3) |

## Web UI

Same pages and look, new internals (D11).

| | Capability | 2.0 | M | Note |
|---|---|---|---|---|
| [ ] | Home: artwork preview loaded from its source, info panel, previous and next, react, pin, pause | keep | M5.1 | |
| [ ] | Home: now-playing card with shuffle toggle, playset stats, per-channel rows with refresh state and museum markers | keep | M5.1 | |
| [ ] | Home: pill bar of playsets, pin lists and built-ins; pin manage and pin browse dialogs | keep | M5.1 | |
| [ ] | Home: upload by drag and drop with progress; show-URL box | keep | M5.1 | |
| [ ] | Home: banners for update, card failure, card repair, rate limits, invalid or missing keys, registration needed, museum unavailable | keep | M5.1 | |
| [ ] | Settings, Display: theme (5, per browser), brightness, rotation, background colour, boot animation and duration, indicator size, FPS, max speed, auto-advance, channel selection, refresh override | keep | M5.1 | Rendered from the schema |
| [ ] | Settings, Network: status, device name, erase Wi-Fi | keep | M5.1 | |
| [ ] | Settings, Giphy, Klipy, Museum, Storage, Makapix, System tabs with their 1.x controls | keep | M5.1 | |
| [ ] | Playset editor: list, play, edit, delete, rename; channel cards with reorder, weight, offset, balance bar; add-channel dialog for every source; museum browse; Makapix user and hashtag check | keep | M5.1 | |
| [ ] | Update page: versions, release notes, install, rollback, check, pre-release markers | keep | M5.1, M5.2 | |
| [ ] | Update page: web UI section, repair, recovery page, "outdated" banner | drop | | Gone with change A |
| [ ] | Setup pages: form with optional device name, success page with countdown, erase, error | keep | M1.5 | Served from the embedded table |
| [ ] | Installable as an app: manifest named after the device, icons, theme colour | keep | M5.1 | Icons shrunk |
| [ ] | Old addresses `/config/network` and `/giphy` redirect into Settings | drop | | |
| [ ] | PICO-8 page | drop | | C5 |

## What a client can do over the API

| | Capability | 2.0 | M | Note |
|---|---|---|---|---|
| [ ] | Status: state, uptime, memory, firmware, hardware, network, Makapix, storage, card health, repair and reboot counters | keep | M1.5 onwards | Free-space figure cached, not computed per request |
| [ ] | One call that returns everything the home page needs | keep | M5.1 | |
| [ ] | Read and write settings, with partial update | change | M1.4, M1.5 | Validated by the schema; unknown keys rejected; one settings surface |
| [ ] | The settings read returns stored API keys in clear | decide | M1.5 | Q2 |
| [ ] | Actions: next, previous, pause, resume, reboot, reset the advance timer | keep | M2.3 | |
| [ ] | Actions: show URL, swap to, react, pin, unpin (checked against what is on screen) | keep | M4.4, M4.5 | |
| [ ] | Actions: start or cancel registration, unregister, reset the Giphy analytics id | keep | M4.2, M4.5 | |
| [ ] | Rotation read and set | keep | M2.4 | Sets; never toggles |
| [x] | Intro animation list and choice | keep | M3.3 | The choice is the `intro_animation` setting (M3.3); listing the names over the API is to do |
| [ ] | Device name read and set | keep | M1.5 | Through settings |
| [ ] | Memory breakdown | keep | M1.5 | |
| [ ] | Museum cooldown table and browser 429 report | keep | M4.3 | |
| [ ] | Update: status, check, install, rollback | keep | M5.2 | Install becomes a job |
| [ ] | Erase Wi-Fi credentials and reboot | keep | M1.5 | |
| [ ] | Legacy channel switch, deprecated channel read, channel stats, UI config, network status, state snapshot, storage, dwell-time endpoints | drop | | Finding 16: unused or duplicated |
| [ ] | Frame-trace endpoints in trace builds | keep | M2.1 | |
| [ ] | Raw SD debug endpoints behind their own option | decide | M1.3 | Q8 |
| [ ] | The API starts only after the first successful Wi-Fi connection | keep | M1.5 | |

## Makapix

| | Capability | 2.0 | M | Note |
|---|---|---|---|---|
| [ ] | Registration by 6-character code; credentials held in RAM until complete so a cancel never damages a working registration | keep | M4.5 | |
| [ ] | MQTT over mutual TLS with per-device certificates; status every 30 s and a last-will "offline" | keep | M4.5 | |
| [ ] | Commands: next, previous, set background colour, play channel, show artwork, show URL, swap to, execute playset (optionally saving it) | keep | M4.5 | Through `player_commands` |
| [ ] | Acknowledged commands: set paused, set brightness, set rotation; "unsupported" for mirror; retained capabilities and state | keep | M4.5 | |
| [ ] | Requests: get post, query posts with cursor and sort, submit and revoke reaction, get playset | keep | M4.5 | |
| [ ] | View reporting: first after 5 s, then every 30 s; only Makapix art; not while a screen covers it or while paused | keep | M4.5 | Hangs off `swap_shown` |
| [ ] | Three authentication failures in a row mark the registration invalid | keep | M4.5 | |
| [ ] | Certificate renewal: token bootstrap, renewal window with jitter, forced renewal on verification failure, CA rotation, result in status | keep | M4.5 | |
| [ ] | Unregister from the web UI | keep | M4.5 | |
| [ ] | Commands dropped during the co-processor update | keep | M4.5 | |

## Storage

| | Capability | 2.0 | M | Note |
|---|---|---|---|---|
| [ ] | Configurable SD root, validated, reboot to apply | keep | M1.3, M1.4 | Validation and resolution done and host-tested (M1.3); the setting itself arrives with M1.4 |
| [ ] | Atomic writes; health latch (boot probe or three failed writes) that stops downloads until reboot | keep | M1.3 | Atomic writes and the latch verified on the device; the content task downloads nothing onto a failed card (M4.1, not tested on the device) |
| [ ] | Self-repair: quarantine to `lost/`, FAT mirror repair at boot, counters | keep | M1.3 | Moved; the boot-time FAT comparison runs. The repair paths need a failing card and are untested in 2.0 |
| [ ] | Eviction: watermarks scaled to the card, age halving from 30 days to a 4-hour floor, needs a synced clock, never touches local files or pins | change | M4.1 | Same marks, floor and clock rule; oldest first in one walk instead of age halving (D65). Local files are not in the blob store. The walk is device-tested through a hook; starting by itself on a full card is not. Pins join the kept set at M4.4 |
| [ ] | Stale channel indices dropped after 60 days | keep | M4.1 | Built (once per boot, the active playset's kept). Not tested |
| [ ] | Five cache trees and a pinned tree on the card | change | M2.2 | One blob store |
| [ ] | USB mass storage: card exposed to a computer; swaps, upload, show URL and updates blocked meanwhile; every session ends in a reboot | keep | M3.6 | |
| [ ] | Build-time switches to start with fresh NVS, Wi-Fi or card | keep | M1.4 | |
| [ ] | Factory reset (new in 2.0, D28) | new | M1.4, M1.5, M5.1 | Store side done and device-tested |

## Network and system

| | Capability | 2.0 | M | Note |
|---|---|---|---|---|
| [ ] | Setup network `p3a-setup` with DNS capture and OS sign-in sheet | keep | M1.5 | Built and device-tested from the inside; a phone joining is manual check M1.5-a |
| [x] | Three first-connection attempts, then setup mode; after a first success, retry forever with back-off | changed | M1.5 | D35: three tries for a network that refuses, two minutes for one that is absent. Device-tested both ways |
| [ ] | Health monitor, reconnect, reinit, and a single reboot when the Wi-Fi chip is stuck, then degraded offline playback | keep | M1.5 | |
| [ ] | SDIO link failure: countdown and reboot, degraded mode after three | keep | M1.5 | |
| [ ] | Names: `p3a` or `p3a-{name}` for mDNS, DHCP, the setup-mode alias, the installed-app name and the screens | keep | M1.5 | |
| [ ] | UTC clock from SNTP; connectivity ladder from no Wi-Fi to online | keep | M1.5 | |
| [ ] | Firmware update from GitHub Releases: check every 12 h and on request, owner approves, SHA-256 checked, rollback, blocked during USB sharing | keep | M5.2 | New asset name (D12) |
| [ ] | No update check until 12 h after boot | decide | M5.2 | Q3 |
| [ ] | Pre-release channel in developer builds | keep | M5.2 | |
| [ ] | Web UI updated over the air on its own | drop | | Change A |
| [ ] | Co-processor firmware flashed at boot when stale; version lock detected and reported | keep | M1.2 | Link, versions and lock detection verified on the dev unit; the update transfer waits for manual check M1.2-e on another board |
| [ ] | Core dump to flash; one-shot heap report on the first failed allocation | keep | M1.1 | Q6 on reading the dump |
| [ ] | Persisted reboot counters for Wi-Fi, touch and SDIO, cleared by a validated update | keep | M1.4 | In the schema |
| [ ] | Brightness set from the UI and over MQTT | keep | M2.4 | Remembered across reboots in 2.0 (D28); restore at boot is done A changed setting applies at once since M2.4; MQTT at M4.5 |
| [ ] | PICO-8 streaming | drop | | C5 |

## New in 2.0

| | Capability | M |
|---|---|---|
| [ ] | One binary: firmware and web UI update together (A) | M5.1 |
| [ ] | Longer identifiers: no hidden Wellcome terms, no CMA table (B) | M4.3 |
| [x] | Repetition suppression (C) | M2.4 |
| [ ] | Aspect-ratio filter for museum channels (D) | M2.4, M4.3 Predicate, settings and wiring done (M2.4) |
| [ ] | Clock overlay, off by default (idea 1) | M3.5 |
| [x] | Giphy and Klipy branding in the letterbox band, on by default (idea 2) | M4.2, in words (D72); the marks wait on O6 |
| [ ] | Pixel fonts on screen (idea 3) | M3.2 |
| [ ] | Pins without duplicate bytes | M4.4 |
| [x] | Back-off under memory pressure | M4.1 |

## Behaviours to decide

The sweep found places where 1.x behaviour looks accidental. Parity would
copy them; each needs a yes or no from Fab before its milestone. They are
also listed in the ledger.

| Id | 1.x behaviour | Question | Before |
|---|---|---|---|
| Q1 | Brightness is saved by the web UI but never read at boot, so the device always starts at 100 | Decided 2026-11-18 (D28): remembered and restored at boot | done |
| Q2 | Reading settings returns the stored API keys in clear to anyone on the LAN. The browser-side museum browse reads the Harvard and Smithsonian keys this way | Decided 2026-11-18 (D33): always masked; one narrow call for the two museum keys only | done |
| Q3 | The first update check happens 12 hours after boot, never at boot | Add a check shortly after boot? | M5.2 |
| Q4 | Pinned Giphy, Klipy and museum art is upscaled nearest-neighbour, while the same art in its own channel is bilinear. The user guide documents this | In 2.0 the record knows its source, so bilinear would follow the art. Which is wanted? | M4.4 |
| Q5 | There is no factory reset a user can reach; only "erase Wi-Fi" and "unregister" | Decided 2026-11-18 (D28): added, API and web UI. The store side is done; the API call is M1.5 and the button M5.1 | done |
| Q6 | Core dumps are written to flash but nothing can read them without a cable | Decided 2026-11-18 (D34): summary in status, download and erase calls, a warning in the web UI | done |
| Q7 | Giphy rendition, format and prefer-downsized, the view-ack flag and the processing-indicator switch are settable only through the API | Decided 2026-11-18 (D29): kept as settings, hidden from the page | done |
| Q8 | Raw SD debug endpoints and the frame-trace provocations exist behind build options | Carry them into 2.0's test hooks, or leave them in 1.x? | M1.3 |
| Q9 | The Makapix `set_mirror` command answers "unsupported" | Leave as is? | M4.5 |
| Q10 | The dwell limit differs between the settings blob (86,400 s) and the dedicated endpoint (100,000 s) | Working decision (D30): one limit, 86,400 s, from the schema | done |
