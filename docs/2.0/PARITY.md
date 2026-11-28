# 2.0 parity checklist

Part of the [2.0 work stream](README.md). 2.0 ships with everything 1.x
does, minus PICO-8 (decision D4). This page lists what "everything" is, so
that a rewrite cannot quietly lose a behaviour.

**Where the list comes from.** `README.md`, `docs/HOW-TO-USE.md`, and a sweep
of the 1.x code on `main` at `4c9de368` on 2026-11-17 (HTTP routes, MQTT
commands, settings keys, gestures, screens, scheduler rules, web UI
controls). The sweep was done by a read-only agent and has not been checked
line by line. Each row was re-checked against the 1.x code when its
milestone moved it. That code is no longer in `legacy/` (emptied on
2026-11-28); it is on `main`.

**How to use it.** The "2.0" column says what happens to the capability:
`keep` (same behaviour), `change` (same purpose, new shape, described in the
note), `drop`, or `decide` (see the list at the end). "M" is the milestone
that delivers it. Tick the box when the milestone's tests cover the row.
M6.2 requires every row ticked or dropped.

**Where it stands (2026-11-28).** 130 rows ticked, 24 open. Nothing in the
open rows is missing code, except one action that is a question (O22). They
wait on:

- **A manual check** (the ledger's checklist): the setup screen and the
  setup network from a phone (M1.5-a), the BOOT button (M2.4-a), a like
  sent to Makapix by a swipe and as a request (M4.5-b), the card on a real
  computer (M3.6-a).
- **Another board** (M1.2-e, O14): the co-processor update, its screen,
  and the commands dropped while it runs.
- **A failure that cannot be had on demand**, carried over on trust: a
  touch controller that fails at boot, the Wi-Fi and SDIO recovery ladders
  with their countdowns, the card's self-repair.
- **A test or a click nobody has made yet**: the JPEG shrink in a browser,
  a drop from the desktop and the show-URL box, the bar while a URL is
  fetched, show URL and an update refused during a USB session, offsets on
  museum and pinned listings, the stale-index sweep, the clearing of the
  restart counters, a certificate renewal against the server, and removing
  a real Makapix registration (API and page).
- **Fab**: whether "reset the advance timer" is dropped (O22).

M6.2 asks for every row ticked or dropped. The rows carried over on trust
cannot be ticked by any test here; whether they hold the release is Fab's
to say (O23).

HTTP paths are not parity items. The API moves to `/api/v2` with one
envelope and one way to do each thing (review findings 16 and 17), so rows
describe what a client can do, not the 1.x URL.

## Content sources

| | Capability | 2.0 | M | Note |
|---|---|---|---|---|
| [x] | Local files from `{root}/animations/` only | keep | M2.3 | |
| [x] | Upload over HTTP: 16 MiB cap, webp gif jpg jpeg png apng bmp, name reduced to its basename, overwrites, plays at once, refused while the card is shared over USB | keep | M2.3 | Done in M2.3 except: plays at once (M2.4) and the USB refusal (with USB sharing). A name the card cannot hold is refused, not reduced Plays at once since M2.4; refused while a computer has the card since M3.6 |
| [ ] | The home page shrinks oversized JPEGs in the browser before upload | keep | M5.1 | Built (`shrinkIfLarge()` in `home.js`: a JPEG larger than the screen is sent as a PNG of the screen's size). The rule and the limits are node-tested (`test_lib.mjs`); the conversion itself was not seen in a browser |
| [x] | Makapix channels: all, promoted, by user, by hashtag, a user's reactions, single artwork | keep | M4.5 | `test_makapix.py::test_a_registered_player_connects_and_lists_every_kind_of_channel`, live. The single-artwork channel is not carried over (D96): a post is shown with `show_artwork` |
| [x] | Promoted refreshes over public HTTPS on an unregistered device, so a new device plays art with no account | keep | M4.5 | Promoted is listed from the public feed when unregistered (M4.5 part 1) and is in the default playset with the card's files (D90) |
| [x] | `followed_artists` and other playsets fetched from the Makapix server by name | keep | M4.5 | D96. `test_makapix.py::test_the_servers_playset_of_followed_artists_is_asked_for_and_played`, and seen live (nine artists) |
| [x] | Giphy: trending and search channels; rating; country code; refresh interval; cache size | keep | M4.2 | M4.2. Live device tests for trending, search, rating, cache size and interval; country code and offset are in the request (host-tested), not seen on the device |
| [x] | Giphy: rejected-key latch with hourly re-probe, 429 cooldown, analytics id with reset, click registered on swipe up | keep | M4.2 | The waits are the content task's for every source (D66): an hour or a settings change after a refused key, at least 15 minutes after a 429. The id, its reset and the click are device-tested; a 429 was not provoked |
| [x] | Giphy: rendition, format and prefer-downsized, settable through the API only | keep | M4.2 | D29: settings, kept off the page. `test_giphy_parse`, `test_settings_model` |
| [x] | Klipy: GIF or sticker, each as trending, search or category; own key; rating; format; refresh interval; cache size | keep | M4.2 | M4.2. Live device tests for GIF trending and sticker search in both formats; categories are a search by name (host-tested) |
| [x] | Klipy: 36 fixed categories offered in the editor | keep | M5.1 | Seen in the editor in Chrome |
| [x] | Museums: Rijks, V&A, Wellcome, SMK, Harvard, Smithsonian, Cleveland, Minneapolis; AIC present but marked unavailable, cached AIC art still plays | keep | M4.3 | M4.3. Eight play on the device; AIC is in the table, unavailable, and a device test watches for its image server letting the device in again. AIC art already on the card is not a special case: it plays while its blobs last |
| [x] | Museums: Harvard and Smithsonian keys; a channel without its key sleeps and wakes when the key is saved | keep | M4.3 | M4.3. Device-tested: no key, a wrong key, the right key |
| [x] | Museums: Rijks resolved lazily in the background; refresh interval and cache size settings | keep | M4.3 | M4.3. Resolved when the picture is fetched (D75); the settings were already there |
| [x] | Museums: 429 cooldown shared between device and browser | change | M4.3, M5.1 | Each keeps its own: the device's waits are the content task's (D66), the browser's are the page's (`static/museum/request.js`). They ask from different addresses at different times |
| [x] | Museums: long identifiers handled by truncation tables (CMA) and hidden terms (Wellcome) | change | M4.3 | M4.3. Gone: a term is 95 characters |
| [x] | Pin lists: up to 64 lists of 4096; one active list; "My Pins" created and recreated automatically; the active list cannot be deleted | keep | M4.4 | |
| [x] | Pin lists: create, rename, set active, delete, pin, unpin, is-pinned, page through, entry detail, fetch the artwork file, play as a channel | keep | M4.4 | Done in M4.4 over `/api/v2/pins`. Fetching an entry's file for the browser comes with the page (M5.1) |
| [x] | Pinning copies the artwork bytes into the list's folder | change | M4.4 | A pin becomes a reference in the blob store |
| [x] | Pinnable: Makapix, Giphy, Klipy, museum. Not local files | keep | M4.4 | Any record whose bytes are in the store; Makapix is seen at M4.5 |
| [ ] | Show a URL: http or https, 16 MiB, saved to `animations/`, with or without an on-screen progress bar; refused while the card is shared | keep | M4.4 | `test_pins.py` (shown, saved, a bad address refused, refused while paused), host `test_show_url`. Not tested: the bar on the screen while it is fetched, and the refusal while a computer has the card |
| [x] | Swap to a named local file or a specific Makapix post | keep | M4.4 | `test_makapix.py::test_a_post_the_owner_sends_is_fetched_and_shown` (`show_artwork`, `swap_to`), `test_pipeline.py` (a named file). From the site's own button: M4.5-b |

## Playback

| | Capability | 2.0 | M | Note |
|---|---|---|---|---|
| [x] | Animated WebP, GIF, APNG; static PNG, JPEG, BMP; transparency | keep | M2.1 | `test_decoders.py`, `test_player.py::test_a_file_of_each_format_reaches_the_panel`; M2.1-a cleared |
| [x] | Fit to screen with aspect ratio kept, centred, borders in the background colour; the colour also prepaints boot | keep | M2.4 | Done in M2.1 |
| [x] | Bilinear upscaling for Giphy, Klipy and museum art; nearest-neighbour for the rest | keep | M2.4 | The source's table says which (`smooth`); the user's files: O18 |
| [x] | Pinned Giphy, Klipy and museum art plays nearest-neighbour, because the choice is made by channel type | decide | M4.4 | Q4, D78: it follows the art's own source |
| [x] | GIF and WebP loop counts ignored; APNG `num_plays` honored | keep | M2.1 | `test_player.py`: a GIF and a WebP that say "once" play for ever; an APNG that says "twice" holds its last frame |
| [x] | Max-speed playback setting | keep | M2.4 | Done in M2.1 |
| [x] | Auto-advance every 30 s by default; 0 turns it off; the timer resets on manual navigation and on request | keep | M2.4 | |
| [x] | Auto-advance holds while the info screen, the setup screen or pause is up, and while the loader is busy | keep | M2.4, M3.3 | `test_ui.py`, `test_pipeline.py`, host `test_sched_turn` |
| [x] | Pick mode random or recency; channel selection SWRR or stochastic | keep | M2.3 | Host `test_sched_core`. Weighted shares were not measured on the device |
| [x] | Previous walks a history of 32 without disturbing the generator; history survives a playset switch | keep | M2.3 | |
| [x] | History and last-played commit when a swap is requested | change | M2.4 | They commit when the artwork is shown (finding 7) |
| [x] | Pause: black screen, backlight off, views and auto-advance stopped; resume restores brightness; not kept across reboot | keep | M2.4 | Views stop with the view tracker (M4.5) |
| [x] | Rotation 0, 90, 180, 270: persisted, applies to playback, screens and overlays | keep | M2.4, M3.3 | `test_player.py`, `test_ui_drawing.py`, `test_touch.py` |
| [x] | User-requested swaps fail loudly; automatic ones retry silently (3 retries, a small blocklist, a skip budget) | keep | M2.4 | Changed (D55): "next" also tries other artworks and reports only if all fail; a named artwork reports at once. No blocklist: a failed artwork is set aside until its channel is listed again |
| [x] | A corrupt cached file is deleted, at most one per minute; local files are never deleted | keep | M2.4 | Host `test_sched_turn`; a local file is never deleted (`test_pipeline.py`). A damaged blob's deletion has no device test |
| [x] | Artworks the server no longer has are remembered and skipped | keep | M4.1 | One implementation: the record's `gone` flag, set by the content task on a 404. Device-tested with the test source |
| [x] | A high miss rate triggers a verification sweep of what is on the card | change | M2.2 | `test_content.py`: an artwork whose blob went is fetched again |
| [x] | Boot restores the active playset; with none, plays Promoted without saving that choice | keep | M2.3, M4.5 | With none: the card's files and Promoted, not written down (D90). `test_playsets.py`, `test_scheduler.py` |
| [x] | First swap waits for the first available artwork, a finished refresh, or the timer | keep | M2.4 | |
| [x] | One-time "refresh every channel now" switch | keep | M4.1 | `POST /api/v2/player/refresh`. Device-tested |
| [x] | Makapix channels other than Promoted do not refresh on an unregistered device, and the UI says so | keep | M4.5 | `test_makapix.py::test_the_other_channels_need_a_registration`; the page's banner in node |
| [x] | Per-channel refresh failures back off | keep | M4.1 | `sched_refresh.c` (D66). Host-tested; the rate-limit and no-key waits seen on the device |

## Playsets

| | Capability | 2.0 | M | Note |
|---|---|---|---|---|
| [x] | Up to 64 channels, each with a weight and an offset; all-zero weights mean equal shares | keep | M2.3 | D91: every channel loads while the indices fit a memory budget. `test_scheduler.py::test_a_playset_of_many_channels_has_every_one_of_them_loaded` |
| [ ] | Offset honoured for museum, Giphy, local and pinned channels | change | M2.3 | D92: not for the SD card. Giphy's is host-tested; the others' effect on a listing is not tested |
| [x] | Built-in playsets: all Makapix, Promoted, SD card, Giphy trending | keep | M2.3 to M4.5 | The home page's pills (node). "All Makapix" playing waits on M4.5-a |
| [x] | A library of up to 32 saved playsets: list, read, create, update, rename, delete | keep | M2.3 | Rename is save under the new name, then delete the old |
| [x] | Playset names of at most 32 characters; identifiers of at most 32 | change | M2.3 | Identifiers grow to 96 bytes |
| [x] | Switching is asynchronous, newest request wins, and the result is reported with a sequence number and error code | change | M2.3 | The call answers at once and the newest wins; the outcome is each channel's state and error in `GET /player`. No sequence number. `test_player_api.py`, `test_museum.py` |
| [x] | The active playset is readable cheaply (ETag, 304) with its banner flags | keep | M2.3 | Every GET has an ETag (`test_api.py`); the flags are each channel's `error` in `GET /player` |
| [x] | A playset file that fails to load is never deleted | keep | M2.3 | |

## Touch and buttons

| | Capability | 2.0 | M | Note |
|---|---|---|---|---|
| [x] | Tap right or left half: next or previous, in rotated coordinates | keep | M3.4 | |
| [x] | A touch that drifts without becoming a swipe is discarded, not a tap | keep | M3.4 | |
| [ ] | Swipe up: pin; also Like on Makapix art, click on Giphy art. Swipe down undoes it. Error indicator on local files | keep | M3.4, M4.4, M4.5 | Pin, unpin and Giphy's click are device-tested (`test_pins.py`, `test_giphy.py`). A like reaches the source (`test_player_api.py`), but none was ever sent to Makapix: M4.5-b |
| [x] | Two-finger rotate past 45 degrees turns the screen 90 degrees, once per gesture | keep | M3.4 | |
| [x] | Long press (2.25 s): info screen, or the setup screen in setup mode; dismisses any screen | keep | M3.4 | `test_touch.py`, host `test_ui_route`, `test_ui_gesture`. Real fingers: M3.4-a |
| [x] | While paused, only long press works, and it resumes | keep | M3.4 | |
| [x] | While a screen is up, taps reach only the SD-format controls | keep | M3.4 | Taps are ignored under a screen (M3.4); the format controls come with that screen (M3.3) |
| [x] | During registration, long press cancels and returns to playback | keep | M3.4 | D93. `test_makapix.py::test_a_registration_shows_a_code_and_a_cancel_leaves_the_device_as_it_was` (the long press) |
| [x] | During a firmware update, gestures are ignored | keep | M3.4 | |
| [ ] | BOOT button toggles pause; ignored during the co-processor update | keep | M1.2, M2.4 | Pause itself is device-tested (`test_pipeline.py`). The button: manual check M2.4-a. Ignored during the co-processor update: cannot run on the dev unit (M1.2-e) |
| [ ] | Touch failure at boot: countdown and one reboot, then carry on without touch | keep | M3.4 | Carried over on trust: the counters and the rule are tested (`test_settings.py`), the countdown is a screen; a controller that fails cannot be had |

## On-screen UI

| | Capability | 2.0 | M | Note |
|---|---|---|---|---|
| [x] | Boot: background painted before the backlight, then one of 22 intro animations (random or chosen, 1 to 7.5 s) | keep | M3.3 | |
| [x] | Info screen: name and version, uptime, Wi-Fi and signal, Makapix state, Giphy key state, card space, playset counts, web UI address, IP, reboot counters, format button when the card has failed | keep | M3.3 | `test_touch.py` opens it and reads its title and the rows for the web page, firmware, address, card, playset and Makapix; restarts are listed only if there were any. Uptime, Wi-Fi and signal, and the Giphy key are not asserted; by eye: M3.3-a |
| [ ] | Setup screen: network name, both addresses, connected clients; appears when setup mode starts | keep | M3.3 | Both addresses and whether anything has joined. Not asserted by a test: the API cannot be reached from the LAN in setup; M1.5-a sees it |
| [x] | Registration: status, the 6-character code with a colour-coded countdown, success notice for 10 s; expiry leaves the mode | keep | M3.3, M4.5 | `test_makapix.py::test_a_registration_shows_a_code_and_a_cancel_leaves_the_device_as_it_was`; the success notice was seen by Fab when he registered the dev unit (M4.5-a). The countdown's colours are not asserted |
| [x] | Firmware update screen with versions and progress | keep | M3.3, M5.2 | |
| [ ] | Co-processor update screen with reboot countdown | keep | M1.2, M3.3 | The screen is device-tested as a screen (`test_ui.py`); a real update: M1.2-e, on another board |
| [x] | Channel messages: fetching, downloading with progress, failed, empty, loading, waiting for network, empty pin list, no playable files, key needed, museum unavailable, starting | keep | M3.3 | One surface (`ui_player.c`): host `test_ui_player`, `test_ui.py`. No progress figure while downloading |
| [x] | Refresh-error notices with a 20 s life and a cap of two per boot; the Giphy invalid-key notice stays | keep | M4.2 | D71. The refused-key notice is device-tested; the once-only ones are host-tested |
| [x] | "SD card is not working" and "SD card repaired" notices, 20 s, once per boot | keep | M3.3 | |
| [x] | "No Usable SD Card" screen and the two-step on-device format | keep | M3.3 | The device boots on behind the screen in 2.0 |
| [x] | USB screens: card exposed, disconnected and restarting | keep | M3.6 | `test_usb.py`, host `test_usb_session`. A real computer: M3.6-a |
| [ ] | Recovery countdowns for the Wi-Fi chip and the SDIO link, and the offline notice | keep | M1.5 | Carried over on trust: on the screen (`net_restart_after_countdown`), with a notice when the device gives up on the network. Neither ladder can be provoked in a test |
| [x] | "No Internet Connection" screen | drop | | Never shown in 1.x |
| [x] | Overlays: FPS, processing indicator (size setting), reaction icons, pin icons, all following rotation | keep | M3.5 | Done in M3.5; the icons' real triggers come with the sources |
| [x] | All text in DejaVu Sans through uGFX | change | M3.2 | `test_ui_drawing.py`, host `test_ui_text`. By eye: M3.2-a |

## Web UI

Same pages and look, new internals (D11).

| | Capability | 2.0 | M | Note |
|---|---|---|---|---|
| [x] | Home: artwork preview loaded from its source, info panel, previous and next, react, pin, pause | keep | M5.1 | The preview comes from the device, not the source (D83) |
| [x] | Home: now-playing card with shuffle toggle, playset stats, per-channel rows with refresh state and museum markers | keep | M5.1 | |
| [x] | Home: pill bar of playsets, pin lists and built-ins; pin manage and pin browse dialogs | keep | M5.1 | The "Followed" pill waits for M4.5 part 4 |
| [ ] | Home: upload by drag and drop with progress; show-URL box | keep | M5.1 | An upload was clicked in Chrome on the dev unit (2026-11-26). Not seen: a drop from the desktop, and the show-URL box (its call is device-tested) |
| [x] | Home: banners for update, card failure, card repair, rate limits, invalid or missing keys, registration needed, museum unavailable | keep | M5.1 | |
| [x] | Settings, Display: theme (5, per browser), brightness, rotation, background colour, boot animation and duration, indicator size, FPS, max speed, auto-advance, channel selection, refresh override | keep | M5.1 | Rendered from the schema |
| [x] | Settings, Network: status, device name, erase Wi-Fi | keep | M5.1 | |
| [x] | Settings, Giphy, Klipy, Museum, Storage, Makapix, System tabs with their 1.x controls | keep | M5.1 | Makapix registration on the page not clicked yet (M4.5-a) |
| [x] | Playset editor: list, play, edit, delete, rename; channel cards with reorder, weight, offset, balance bar; add-channel dialog for every source; museum browse; Makapix user and hashtag check | keep | M5.1 | Rename is save, then delete. Pin lists can be channels |
| [x] | Update page: versions, release notes, install, rollback, check, pre-release markers | keep | M5.1, M5.2 | Going back to the other firmware at any time; a firmware on trial and one that was undone are said |
| [x] | Update page: web UI section, repair, recovery page, "outdated" banner | drop | | Gone with change A |
| [x] | Setup pages: form with optional device name, success page with countdown, erase, error | keep | M1.5 | `test_setup.py`. Seen from a phone: M1.5-a |
| [x] | Installable as an app: manifest named after the device, icons, theme colour | keep | M5.1 | Icons shrunk |
| [x] | Old addresses `/config/network` and `/giphy` redirect into Settings | drop | | |
| [x] | PICO-8 page | drop | | C5 |

## What a client can do over the API

| | Capability | 2.0 | M | Note |
|---|---|---|---|---|
| [x] | Status: state, uptime, memory, firmware, hardware, network, Makapix, storage, card health, repair and reboot counters | keep | M1.5 onwards | `test_api.py::test_status_describes_the_device` |
| [x] | One call that returns everything the home page needs | change | M5.1 | `GET /api/v2/player` carries what changes by the second; the status, Makapix and the update are asked once a minute, each answered with 304 when unchanged |
| [x] | Read and write settings, with partial update | change | M1.4, M1.5 | `test_api.py`, host `test_settings` |
| [x] | The settings read returns stored API keys in clear | change | M1.5 | D33: masked; the two museum keys have their own call. `test_api.py`, `test_settings.py` |
| [ ] | Actions: next, previous, pause, resume, reboot, reset the advance timer | keep | M2.3 | All but the last: `test_player_api.py`, `test_pipeline.py`, `test_api.py`. "Reset the advance timer" has no call in 2.0 and nothing needs one: a question for Fab (O22) |
| [x] | Actions: show URL, swap to, react, pin, unpin (checked against what is on screen) | keep | M4.4, M4.5 | `test_pins.py`, `test_player_api.py`. Swap-to is a Makapix command only. A like on Makapix art: M4.5-b |
| [ ] | Actions: start or cancel registration, unregister, reset the Giphy analytics id | keep | M4.2, M4.5 | Start and cancel: `test_makapix.py`. The Giphy id: `test_giphy.py`. Unregister (`DELETE /api/v2/makapix`, D97) is tested only as refused on a unit with no registration: removing a real one was not run |
| [x] | Rotation read and set | keep | M2.4 | Through settings: `test_player.py`, `test_api.py` |
| [x] | Intro animation list and choice | keep | M3.3 | The choice is the `intro_animation` setting (M3.3); listing the names over the API is to do |
| [x] | Device name read and set | keep | M1.5 | `test_net.py`, `test_setup.py` |
| [x] | Memory breakdown | change | M1.5 | A summary of the internal pool in the status call (level, free, largest block, lowest). `test_api.py` |
| [x] | Museum cooldown table and browser 429 report | change | M4.3 | No such calls: the device and the browser each keep their own waits (see the museums' row above) |
| [x] | Update: status, check, install, rollback | keep | M5.2 | Install becomes a job |
| [x] | Erase Wi-Fi credentials and reboot | keep | M1.5 | `test_setup.py` |
| [x] | Legacy channel switch, deprecated channel read, channel stats, UI config, network status, state snapshot, storage, dwell-time endpoints | drop | | Finding 16: unused or duplicated |
| [x] | Frame-trace endpoints in trace builds | keep | M2.1 | `test_timing.py` on the trace-test build |
| [x] | Raw SD debug endpoints behind their own option | decide | M1.3 | Not carried over: D94. The row is dropped |
| [x] | The API starts only after the first successful Wi-Fi connection | change | M1.5 | The server starts with the network layer and serves setup too. `test_setup.py` |

## Makapix

| | Capability | 2.0 | M | Note |
|---|---|---|---|---|
| [x] | Registration by 6-character code; credentials held in RAM until complete so a cancel never damages a working registration | keep | M4.5 | The code and the cancel: `test_makapix.py`. Completed once, by Fab by hand, on the dev unit (M4.5-a, 2026-11-26) |
| [x] | MQTT over mutual TLS with per-device certificates; status every 30 s and a last-will "offline" | keep | M4.5 | Live on the registered dev unit (`test_makapix.py`); topics, the status and the will are host-tested (`test_makapix_link_logic`) |
| [x] | Commands: next, previous, set background colour, play channel, show artwork, show URL, swap to, execute playset (optionally saving it) | keep | M4.5 | `test_makapix.py::test_the_owners_commands_are_the_players_own_calls` and `test_a_post_the_owner_sends_is_fetched_and_shown`, host `test_makapix_command_map`. The commands are given to the device by a test hook; from the site's buttons: M4.5-b |
| [x] | Acknowledged commands: set paused, set brightness, set rotation; "unsupported" for mirror; retained capabilities and state | keep | M4.5 | The same tests. `set_mirror` answers "unsupported" as in 1.x (Q9, still Fab's to confirm) |
| [ ] | Requests: get post, query posts with cursor and sort, submit and revoke reaction, get playset | keep | M4.5 | Query posts and get playset: live (`test_makapix.py`). `get_post` is not carried over (D96). A reaction's message is host-tested; none was ever sent: M4.5-b |
| [x] | View reporting: first after 5 s, then every 30 s; only Makapix art; not while a screen covers it or while paused | keep | M4.5 | Host `test_makapix_link_logic` (the times, covered time); live, acknowledged by the server (`test_makapix.py`) |
| [x] | Three authentication failures in a row mark the registration invalid | keep | M4.5 | Host `test_makapix_link_logic` (three failed handshakes, one renewal, then invalid). Not provoked on the device |
| [ ] | Certificate renewal: token bootstrap, renewal window with jitter, forced renewal on verification failure, CA rotation, result in status | keep | M4.5 | Built (`makapix_renew.c`); when one is due is host-tested, and the status reports the certificate's end. A renewal was never made against the server: the dev unit's certificate is new |
| [ ] | Unregister from the web UI | keep | M4.5 | The button is on the settings page and calls `DELETE /api/v2/makapix` (D97). Not clicked: it would remove the dev unit's registration |
| [ ] | Commands dropped during the co-processor update | keep | M4.5 | Built (`net_makapix_commands.c`: dropped unless the command is one that is acknowledged). Cannot run on the dev unit (M1.2-e) |

## Storage

| | Capability | 2.0 | M | Note |
|---|---|---|---|---|
| [x] | Configurable SD root, validated, reboot to apply | keep | M1.3, M1.4 | `test_settings.py`, host `test_sd_path` |
| [x] | Atomic writes; health latch (boot probe or three failed writes) that stops downloads until reboot | keep | M1.3 | `test_storage.py`, host `test_sched_refresh` |
| [ ] | Self-repair: quarantine to `lost/`, FAT mirror repair at boot, counters | keep | M1.3 | Carried over on trust: moved; the boot-time FAT comparison runs. The repair paths need a failing card |
| [x] | Eviction: watermarks scaled to the card, age halving from 30 days to a 4-hour floor, needs a synced clock, never touches local files or pins | change | M4.1 | D65. Host `test_evict`, `test_content.py`, `test_pins.py`. Starting by itself on a full card was not seen |
| [ ] | Stale channel indices dropped after 60 days | keep | M4.1 | Built (once per boot, the active playset's kept). Not tested |
| [x] | Five cache trees and a pinned tree on the card | change | M2.2 | `test_library.py`, `test_fetch.py`, `test_pins.py` |
| [ ] | USB mass storage: card exposed to a computer; swaps, upload, show URL and updates blocked meanwhile; every session ends in a reboot | keep | M3.6 | `test_usb.py` with the bus's reports injected (the screen, playback stopped, upload, next and a playset save refused, the restart), host `test_usb_session`. Show URL and an update during a session are not tested. A real computer: M3.6-a |
| [x] | Build-time switches to start with fresh NVS, Wi-Fi or card | drop | M1.4 | Not carried over: `lab.py erase`, the factory reset and a release's `nvs-erased.bin` do it without a special build |
| [x] | Factory reset (new in 2.0, D28) | new | M1.4, M1.5, M5.1 | Store and API device-tested (`test_settings.py`, `test_api.py`). The button on the settings page was not clicked |

## Network and system

| | Capability | 2.0 | M | Note |
|---|---|---|---|---|
| [ ] | Setup network `p3a-setup` with DNS capture and OS sign-in sheet | keep | M1.5 | Built and device-tested from the inside; a phone joining is manual check M1.5-a |
| [x] | Three first-connection attempts, then setup mode; after a first success, retry forever with back-off | changed | M1.5 | D35: three tries for a network that refuses, two minutes for one that is absent. Device-tested both ways |
| [ ] | Health monitor, reconnect, reinit, and a single reboot when the Wi-Fi chip is stuck, then degraded offline playback | keep | M1.5 | Carried over on trust (M1.5): a Wi-Fi chip that is stuck cannot be had on demand |
| [ ] | SDIO link failure: countdown and reboot, degraded mode after three | keep | M1.5 | Carried over on trust (M1.5): the counters and the rule are tested (`test_settings.py`), the failure cannot be provoked |
| [x] | Names: `p3a` or `p3a-{name}` for mDNS, DHCP, the setup-mode alias, the installed-app name and the screens | keep | M1.5 | `test_net.py` (hostname, mDNS), `test_web.py` (the installed app's name), `test_touch.py` (the screens). The setup-mode alias: `test_setup.py` |
| [x] | UTC clock from SNTP; connectivity ladder from no Wi-Fi to online | keep | M1.5 | `test_net.py`, host `test_clock` |
| [x] | Firmware update from GitHub Releases: check every 12 h and on request, owner approves, SHA-256 checked, rollback, blocked during USB sharing | keep | M5.2 | `p3a-v2.bin` (D85); first look two minutes after the network (D87); a real GitHub install is manual check M5.2-a |
| [x] | No update check until 12 h after boot | decide | M5.2 | Changed: D87 |
| [x] | Pre-release channel in developer builds | change | M5.2 | A setting, `update_prereleases`, in every build (D32) |
| [x] | Web UI updated over the air on its own | drop | | Change A |
| [ ] | Co-processor firmware flashed at boot when stale; version lock detected and reported | keep | M1.2 | Link, versions and lock detection verified on the dev unit; the update transfer waits for manual check M1.2-e on another board |
| [x] | Core dump to flash; one-shot heap report on the first failed allocation | keep | M1.1 | D34. `test_api.py`, `test_events_and_memory.py` |
| [ ] | Persisted reboot counters for Wi-Fi, touch and SDIO, cleared by a validated update | keep | M1.4 | Cleared when an updated firmware is kept (`p3a_recovery_clear`). The clearing itself has no test |
| [x] | Brightness set from the UI and over MQTT | keep | M2.4 | `test_settings.py`, `test_board_display.py`, `test_makapix.py` (the command, by hook). Over MQTT: M4.5-a |
| [x] | PICO-8 streaming | drop | | C5 |

## New in 2.0

| | Capability | M |
|---|---|---|
| [x] | One binary: firmware and web UI update together (A) | M5.1 |
| [x] | Longer identifiers: no hidden Wellcome terms, no CMA table (B) | `test_museum.py`, node `test_museum`, host `test_record_index` |
| [x] | Repetition suppression (C) | M2.4 |
| [x] | Aspect-ratio filter for museum channels (D) | M2.4, M4.3. Seen on the device with a 5:1 picture: passed over with the filter on, played with it off |
| [x] | Clock overlay, off by default (idea 1) | `test_overlays.py`, host `test_ui_clock`. By eye: M3.5-a |
| [x] | Giphy and Klipy branding in the letterbox band, on by default (idea 2) | M4.2; the services' own marks since D101 |
| [x] | Pixel fonts on screen (idea 3) | `test_ui_drawing.py`, host `test_ui_text`. By eye: M3.2-a |
| [x] | Pins without duplicate bytes | `test_pins.py`, host `test_pin_table` |
| [x] | Back-off under memory pressure | M4.1 |

## Behaviours to decide

The sweep found places where 1.x behaviour looks accidental. Parity would
copy them; each needs a yes or no from Fab before its milestone. They are
also listed in the ledger.

| Id | 1.x behaviour | Question | Before |
|---|---|---|---|
| Q1 | Brightness is saved by the web UI but never read at boot, so the device always starts at 100 | Decided 2026-11-18 (D28): remembered and restored at boot | done |
| Q2 | Reading settings returns the stored API keys in clear to anyone on the LAN. The browser-side museum browse reads the Harvard and Smithsonian keys this way | Decided 2026-11-18 (D33): always masked; one narrow call for the two museum keys only | done |
| Q3 | The first update check happens 12 hours after boot, never at boot | Add a check shortly after boot? | M5.2: working answer D87, two minutes after the network comes up |
| Q4 | Pinned Giphy, Klipy and museum art is upscaled nearest-neighbour, while the same art in its own channel is bilinear. The user guide documents this | Decided 2026-11-25 (D78), for Fab to overturn: the scaling follows the art's own source | M4.4 |
| Q5 | There is no factory reset a user can reach; only "erase Wi-Fi" and "unregister" | Decided 2026-11-18 (D28): added, API and web UI. The store side is done; the API call is M1.5 and the button M5.1 | done |
| Q6 | Core dumps are written to flash but nothing can read them without a cable | Decided 2026-11-18 (D34): summary in status, download and erase calls, a warning in the web UI | done |
| Q7 | Giphy rendition, format and prefer-downsized, the view-ack flag and the processing-indicator switch are settable only through the API | Decided 2026-11-18 (D29): kept as settings, hidden from the page | done |
| Q8 | Raw SD debug endpoints and the frame-trace provocations exist behind build options | Decided 2026-11-25 (D94), for Fab to overturn: not carried over | done |
| Q9 | The Makapix `set_mirror` command answers "unsupported" | Leave as is? | M4.5 |
| Q10 | The dwell limit differs between the settings blob (86,400 s) and the dedicated endpoint (100,000 s) | Working decision (D30): one limit, 86,400 s, from the schema | done |
