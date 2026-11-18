# 2.0 progress ledger

Part of the [2.0 work stream](README.md). This is the first page to read in
any session and the last to update in any checkpoint. Keep "Now" true at all
times.

## Now

| | |
|---|---|
| Phase | 2, player and library (Phases 0 and 1 complete) |
| Current milestone | M2.1 display pipeline and decoders |
| Next action | M2.1 part 2, the display pipeline: read `docs/infrastructure/display-pipeline.md` and `docs/jitter/REPORT.md` (fix 3, band-stealing upscale, and the Phase 6 catch-up policy live in these files and move untouched), then `legacy/main/display_renderer.c`, `display_renderer_priv.h`, `display_upscaler.c`, `display_ppa_upscaler.c`. Bring them into a `player`-layer component with the frame trace calls intact; audit M10 (the parallel upscaler waits with no deadline) is due here. Test hook: push decoded frames from the corpus through the renderer and compare the HTTP screenshot with a reference upscaled on the workstation. Then part 3: the animation player's decode path, the frame-trace endpoints, `host/jitter-lab` adapted, a trace baseline. Part 3 also decides whether end-of-animation becomes uniform across formats (see the M2.1 part 1 entry) |
| Blocked on | Nothing |
| In flight | Nothing |
| Uncommitted work | None |

## Dev unit

| | |
|---|---|
| Running | `2.0.0-dev`, test configuration (`build-test`), flashed 2026-11-18 by the device tests. On the Wi-Fi as `p3a-fab.local` |
| Flash state | Bootloader, 2.x partition table, app in `ota_0`, the 2.9.3 co-processor image in `slave_fw`. NVS holds the test rig's Wi-Fi credentials and the device name `fab`, put there by the lab (`lab.py provision`). The C6 itself runs esp_hosted 2.7.0 and cannot be updated (see M1.2-e) |
| SD card | 32 GB, healthy (D8). Still holds 1.x folders; see the M1.3 entry in the session log |
| 1.x flash backup | `%USERPROFILE%\.p3a-lab\backups\p3a-fab_v1.2.4_full-flash_2026-11-17.bin` (see the session log for size and hash) |
| Secrets file | Complete: four API keys read from the unit, Wi-Fi password filled in by Fab |

## Milestones

Status is one of: not started, in progress, done (date, commit, tiers),
reopened (why).

| Milestone | Status |
|---|---|
| M0.1 Tree reset and empty app | done 2026-11-17. Tiers: local release build; flashed, boot log read over UART. No CI yet (M0.3), no test configuration yet (M0.4) |
| M0.2 Host test harness | done 2026-11-17. Tiers: host tests pass with MinGW gcc 16.1 and with gcc 13.3 under ASan and UBSan in WSL; firmware builds with the new component |
| M0.3 Continuous integration | done 2026-11-17. Tiers: CI green on GitHub for tree checks, host tests and the firmware build; each linter rule proven to fail by its self-test (29 cases). No deliberately broken commit was pushed |
| M0.4 Device lab | done 2026-11-17. Tiers: device tests (6 cases, run four times), lab unit tests (4 cases), both builds checked. Provisioning waits for M1.4 and M1.5 |
| M1.1 util | done 2026-11-17. Tiers: host tests (6 programs, under ASan and UBSan too), device tests (21 cases, run twice), both builds checked, CI. `p3a_limits.h` is not created yet: it arrives with its first constant |
| M1.2 board | done 2026-11-17. Tiers: device tests (47 cases, twice), release, test and trace builds, the configuration guard, CI. Awaiting manual checks M1.2-a to d. The co-processor update itself cannot run on the dev unit (its C6 is on the unupdatable 2.7.0); see M1.2-e |
| M1.3 storage | done 2026-11-18. Tiers: host tests (8 programs, under ASan and UBSan too), device tests (62 cases), release and test builds, the configuration guard. The repair paths of `sd_repair` cannot be exercised on a healthy card and are carried over on trust |
| M1.4 settings | done 2026-11-18. Tiers: host tests (9 programs, under ASan and UBSan too), generator self-test (15 cases), device tests (76 cases, 14 of them new), release and test builds, the configuration guard. The touch-recovery restart itself is not exercised (the controller works); its counters and rule are |
| M1.5 net base | done 2026-11-18. Tiers: host tests (12 programs, under ASan and UBSan too), tool self-tests, device tests (119 cases, on the real Wi-Fi), release and test builds, the configuration guard. Awaiting manual check M1.5-a (the portal from a phone). The Wi-Fi recovery ladders are carried over untested |
| M2.1 Display pipeline and decoders | in progress. Part 1 done 2026-11-18 (the decoders): host tests (13 programs, under ASan and UBSan too), device tests (202 cases, 83 of them the decoder corpus), both builds, the configuration guard. Parts 2 and 3 (display renderer and upscalers; the player's decode path, frame trace and jitter lab) not started |
| M2.2 Library core | not started |
| M2.3 Source interface, local source, scheduler | not started |
| M2.4 Loader pipeline, C and D | not started |
| M3.1 Screen model and hook table | not started |
| M3.2 Text and fonts | not started |
| M3.3 Screens | not started |
| M3.4 Touch router and gestures | not started |
| M3.5 Overlays | not started |
| M3.6 USB mass storage | not started |
| M4.1 Fetch, download, refresh, eviction | not started |
| M4.2 Giphy and Klipy | not started |
| M4.3 Museums | not started |
| M4.4 Pinned lists and "show this" | not started |
| M4.5 Makapix | not started |
| M5.1 Embedded web UI | not started |
| M5.2 Firmware OTA for 2.x | not started |
| M5.3 Release engineering | not started |
| M5.4 Documentation | not started |
| M6.1 Soaks | not started |
| M6.2 Parity audit | not started |
| M6.3 Field test | not started |
| M6.4 Release | not started |

## Manual checklist

Checks only Fab can do (D10). Each line: id (milestone plus a letter), what
to do, what should happen. Fab ticks a line or reports what he saw; a failed
check reopens its milestone.

Open:

- **M1.2-a, the panel shows what the framebuffer holds.** Run
  `python host/p3a-lab/lab.py cmd "lcd bars"`. The panel should show eight
  vertical bars of equal width, left to right: white, yellow, cyan, green,
  magenta, red, blue, black. Edges sharp, colours pure, no flicker or
  tearing. Then `python host/p3a-lab/lab.py cmd "lcd gradient"`: a smooth
  gradient with a thin white border on all four edges and a red square in
  the top-left corner (top-left as the board is normally held, USB ports at
  the bottom).
- **M1.2-b, the backlight dims.** Run
  `python host/p3a-lab/lab.py cmd "lcd brightness 20"`: clearly dimmer.
  `... "lcd brightness 0"`: backlight off. `... "lcd brightness 100"`: full
  again.
- **M1.2-c, the BOOT button is seen.** Run
  `python host/p3a-lab/lab.py cmd button` (prints `"presses": 0` or the
  count so far), press BOOT (the top button) twice, run it again: the count
  is two higher.
- **M1.2-d, the touch controller sees a finger.** Hold a finger on the
  panel and run `python host/p3a-lab/lab.py cmd touch`: it should print
  `"points": 1` with `x` and `y` near where the finger is (0,0 is the
  top-left corner, 719,719 the bottom-right). Without a finger it prints
  `"points": 0`.
- **M1.2-e, a co-processor update on a unit that can take one.** Needs a
  board whose ESP32-C6 does not run esp_hosted 2.7.0 (a factory-fresh board
  reports 0.0.0), so it waits for the second board (O14). Flash 2.0 on it
  and watch the UART log at first boot: it should say "Co-processor
  firmware update required", count progress to 100 percent, restart, and
  on the next boot report 2.9.3 and "up to date". The screen part of this
  check comes later, with the UI.
- **M1.4-a, stored brightness and background colour are applied at boot.**
  Run
  `python host/p3a-lab/lab.py cmd "settings set {\"brightness\":15,\"background_color\":\"#003060\"}"`
  and then `python host/p3a-lab/lab.py cmd reboot`. The panel should come
  up dark blue and clearly dim. Put it back with
  `python host/p3a-lab/lab.py cmd "settings factory-reset"` and another
  reboot: black and bright again.
- **M1.5-a, setup from a phone.** Put the unit in setup with
  `python host/p3a-lab/lab.py cmd "net forget"` and
  `python host/p3a-lab/lab.py cmd reboot`. On a phone, join the Wi-Fi
  network `p3a-setup` (no password). The phone should open the setup page
  by itself within a few seconds (its "sign in to network" sheet); if it
  does not, open `http://192.168.4.1/`. Check that the page looks right,
  type the Wi-Fi name and password and the device name `fab`, and press
  "Save & connect": the page should say "Saved! Rebooting now" with
  `http://p3a-fab.local/` as the address, and within about 15 seconds
  `python host/p3a-lab/lab.py cmd "net status"` should report `"up": true`.
  Worth trying on both an iPhone and an Android phone if both are at hand.
  Also worth one try with a wrong password: the unit should offer
  `p3a-setup` again within about a minute. If anything goes wrong,
  `python host/p3a-lab/lab.py provision` puts the unit back online.

Cleared:

- None yet.

## Open questions for Fab

Decisions that are still open, with the milestone that needs them. Details
and proposals are in [DECISIONS.md](DECISIONS.md) under "Open decisions".

| Id | Question | Needed before |
|---|---|---|
| Q1 to Q10 | Ten 1.x behaviours that look accidental (brightness not restored at boot, API keys readable over the LAN, no update check for 12 hours, and others). Keep or change each? Listed in [PARITY.md](PARITY.md) under "Behaviours to decide" | M1.3 at the earliest; each row names its milestone |
| O17 | Two picture-quality bugs found in 1.x while testing the 2.0 decoders are in every unit in the field: hardware-decoded JPEGs have about 16% too much contrast with clipped shadows and highlights, and animated WebP loses the colour of half-transparent pixels. Both fixes are small and self-contained. Port them to `main` for 1.2.5? | 1.2.5 |
| O2 | Should 1.2.5 carry a notice about 2.0, and with what wording? | 1.2.5 |
| O4 | Which VEXED font families and sizes? | M3.2 |
| O3 | Keep uGFX for text, or replace it with a glyph-sheet renderer? | M3.2 |
| O5 | Clock overlay details (12 or 24 hour, seconds, when hidden) | M3.5 |
| O6 | Branding overlay rules from the Giphy and Klipy terms | M3.5 |
| O15 | Does Makapix need any server-side change for 2.0 devices? | M4.5 |
| O1 | Exact name of the 2.x firmware asset | M5.2 |
| O12 | OTA hardening choices | M5.2 |
| O14 | A second board for the field test | M6.3 |
| O13 | How `v2` becomes `main` | M6.4 |

The remaining open decisions (O7, O9, O10) may be taken as working decisions
by the agent and are reported when taken.

## Ports from main

Fixes made on `main` after `v2` branched (at `4c9de368`) that 2.0 must pick
up by hand. Most 1.x fixes will not apply because the code is being
rebuilt; list them anyway and mark each "ported", "not applicable" with the
reason, or "pending".

| Commit on main | What | Status in 2.0 |
|---|---|---|
| (none yet) | | |

Expected: the lock-order fix for finding 25 (D9). In 2.0 the lifecycle lock
it concerns should not exist; confirm at M4.1 and mark it not applicable.

## Deletion log

What was deleted from `legacy/` without being moved, and why. Filled from
M0.1 on.

M0.1, 2026-11-17 (paths relative to the 1.x tree):

- PICO-8 (decision C5): `components/pico8/`, `webui/pico8/`,
  `components/http_api/http_api_pico8.c` and `pico8_logo_data.h`,
  `main/playback_controller.c` and its header,
  `scripts/convert_pico8_logo.py`.
- Dead code (finding 23): `components/content_cache/`,
  `components/playback_queue/`; in `channel_manager`: `channel_interface.h`,
  `sdcard_channel_impl.c/.h`, `playlist_manager.c/.h`,
  `channel_settings.c/.h`; in `makapix`: `makapix_channel_switch.c`,
  `makapix_single_artwork.c`. `makapix_channel_switch.c` also held the live
  `makapix_show_artwork`, a thin call into the scheduler; M4.5 writes its
  equivalent.
- Forwarding services (finding 13): `main/connectivity_service.c`,
  `content_service.c`, `render_engine.c` and their headers.
- Web UI update stack (change A):
  `components/ota_manager/ota_manager_webui.c`,
  `components/http_api/surrogate_ui.h`, `webui/static/compat.js` and
  `compat.js.in`, `webui/metadata.json`.

Dead functions inside files that are otherwise live were not hunted down;
they are simply not moved when their file is.

M2.1, 2026-11-18:

- `animation_decoder_decode_next` (RGBA) and its five per-format
  implementations: no callers, and unsafe (audit H14).
- `components/animation_decoder/Kconfig`: its one option is a constant.
- `debug_http_log` calls and the `CONFIG_P3A_PERF_DEBUG` blocks in the WebP
  decoder: profiling code for a build option nothing sets.
- `animation_pixel_format_t` and the `pixel_format` field: every decoder
  reported the same value.
- The mutual requirement between `animation_decoder` and
  `animated_gif_decoder`.

M1.5, 2026-11-18:

- `legacy/webui/setup/success.html`, `error.html`, `erased.html`: their
  content became states of the one setup page
  (`components/p3a_net/static/setup/index.html`, moved from the 1.x
  `index.html`).
- `legacy/components/wifi_manager/CMakeLists.txt` and `Kconfig`: every
  source of that component has moved to `components/p3a_net`. Its two
  build options (`WIFI_MANAGER_ENABLED`, `WIFI_MANAGER_SNTP_SERVER`) were
  read by nothing.
- From the portal code: the form-encoded `/save` and `/erase` handlers and
  their hand-written parser (replaced by `PUT` and `DELETE /api/v2/wifi`
  with JSON), the server-side `{SSID}`, `{HOSTNAME}` and `{DEVICE_NAME}`
  substitution in pages, and the second HTTP server.

M1.4, 2026-11-18 (not carried over from `config_store`; the files stay in
`legacy/` until their last readers move):

- `randomize_playlist`: stored and read back by nothing else in 1.x.
- `giphy_random_id`: device state, not a setting. It returns with the Giphy
  source (M4.2) in that source's own storage.
- `refresh_allow_override`: never persisted in 1.x; run-time state of the
  refresh scheduler (M4.1).
- The build-time default API keys (`CONFIG_GIPHY_API_KEY_DEFAULT`,
  `CONFIG_KLIPY_API_KEY_DEFAULT`): a key is never compiled into a binary.
- The per-key caches, generation counters and 95 accessor functions: the
  typed struct replaces them.

M1.3, 2026-11-18:

- From `sd_path`: the per-store path getters of the 1.x layout (`vault`,
  `channel`, `playlists`, `giphy`, `klipy`, `museum`, `pinned`) and the
  creation of those folders at boot. 2.0 has one blob store and one index
  folder, defined at M2.2; each store creates its own folders on first use.
  `sd_path` keeps the root, `animations`, `temporary`, name sanitizing and
  the shard layout.

M1.2, 2026-11-17:

- `components/p3a_board_ep44b/p3a_board_fs.c`: the LittleFS mount and the
  web UI health check. Gone with change A.
- `components/p3a_board_ep44b/CMakeLists.txt` and `Kconfig`: replaced by
  the new component's. The Kconfig held the pixel-format choice, the
  capability flags (`P3A_HAS_*`) and the button GPIO; 2.0 targets one board
  with one format, so none of them is an option any more (audit M28).

M1.1, 2026-11-17:

- `components/event_bus/` (342 lines). Rewritten as `p3a_event` in
  `components/p3a_util` rather than moved: the 2.0 bus drops categories,
  runs handlers outside its lock, counts dropped events, and takes its
  event types from a new list that starts almost empty. The 18 event types
  of 1.x are not carried over; each returns only if its emitter arrives and
  it is a real fan-out (D22).

## Session log

Newest first. One entry per session or checkpoint: date, what was done,
what was verified and at which tier, the commit.

### 2026-11-18: M2.1 part 1, the decoders, and two 1.x picture bugs

- Fab confirmed the direction after Phase 1: keep the improvements and
  polish coming, ask when in doubt. He decided D37 (short frame delays play
  as browsers play them).
- Moved out of `legacy/components` with `git mv`:
  - `animation_decoder` to `components/animation_decoder` (layer `player`),
    with `gif_animation_decoder.cpp` joining it from `animated_gif_decoder`.
  - `animated_gif_decoder` to `components/animated_gif`: now only the
    vendored AnimatedGIF library. The two components used to require each
    other; that cycle is gone.
  - `libwebp_decoder` (unchanged: libwebp 1.4.0 fetched by tag at build
    time) and `espressif__libpng` to `components/libpng` (the APNG fork;
    604 files, marked vendored).
- What changed on the way in:
  - The dispatcher. 1.x had it inside the WebP file, as seven functions of
    `if (type == ...)` chains. It is now `animation_decoder.c`: one table of
    operations per format. The WebP decoder is a format like the others.
  - The unused RGBA entry point is deleted from the interface and from all
    five decoders (audit H14: it wrote four bytes per pixel into buffers
    sized for three).
  - Sizes from a file are checked before anything is computed from them
    (audit H1). One limit for every format,
    `ANIMATION_DECODER_MAX_DIMENSION` (4096 a side): JPEG hardware path,
    JPEG software path after scaling, PNG, GIF, WebP, BMP. PNG and GIF had
    no limit and could wrap a buffer size on a crafted file.
  - The decoders no longer reach up into the configuration store for the
    background colour. The component holds it
    (`animation_decoder_set_background`), takes it from the
    `background_color` setting at start and follows the change event.
  - Allocation goes through `psram_alloc.h`. The Kconfig option for the
    still-frame delay is a constant. The dead profiling code in the WebP
    path is gone.
  - Still needed and kept: the link-time wrapper for the IDF's
    `jpeg_release_codec_handle` (the bug is still in v5.5.4) and the alias
    the managed libjpeg-turbo component needs to configure.
- **The test corpus** (`tests/assets/decoders/`, made by
  `tests/assets/make_corpus.py`, deterministic, 270 KB): 39 good files
  covering the kinds each decoder handles (PNG colour types, APNG dispose
  and blend modes, GIF disposal methods, WebP lossy, lossless, still and
  animated, JPEG baseline, progressive, grey, subsampled and larger than
  the screen, BMP depths) and 41 bad ones (truncated, damaged, not images,
  and headers claiming up to 100,000 pixels a side).
- **The reference is an independent decoder**, Pillow, on the workstation:
  the device test fetches each decoded frame over HTTP and compares pixels.
  For APNG the test composites frames itself from the specification,
  because Pillow's reader is not exact where a translucent pixel is drawn
  over another (the device is).
- **Two bugs in 1.x found this way**, both in every unit in the field:
  1. *Hardware JPEG colour.* The IDF's hardware JPEG path converts to RGB
     with video-range coefficients (16 to 235), and a JPEG file is full
     range. Every baseline JPEG had about 16% too much contrast, with
     everything below 16 crushed to black and above 235 clipped to white.
     Measured: 8 to 13 levels of mean error per channel against libjpeg.
     Fix (D38): ask the hardware for YCbCr and convert in software with
     libjpeg's own constants. Now 0.06 levels with full-resolution colour.
     With subsampled colour the mean is 4.8 on a deliberately harsh test
     picture, because the hardware repeats colour samples where libjpeg
     interpolates; brightness matches to 0.02.
  2. *Animated WebP with partial transparency.* The blend added the
     premultiplied colour before dividing by 255 instead of after, which
     discarded it: soft edges came out as darkened background. Errors of up
     to 160 levels; now within rounding.
  Whether to port them to 1.2.5 is open question O17.
- A behaviour change, D37: a declared frame delay of 10 ms or less plays as
  100 ms, in every format, as in every browser. This also gives a
  single-frame GIF the 100 ms of every other still (it reported 1 ms).
- Test hook `POST /api/v2/test/decode`: send a file, get its description
  (per-frame CRC, delay and decode time) or one frame's pixels. Decoding
  runs on its own task.
- The BMP decoder, p3a's own parser, also runs on the workstation under the
  sanitizers: the corpus, every truncation of every good file, every header
  byte at five extreme values, and 15,000 randomly damaged copies. Silent.
- Numbers from the device: every decoder stays within 3.1 KB of stack. The
  1440 by 1440 progressive JPEG decodes (scaled to 720) in 650 ms. The test
  image is 1.39 MB with the decoders linked; the release image is 900 KB,
  because nothing in a release build calls them until the player arrives.
- Verified: 13 host test programs with MinGW and under ASan and UBSan; 83
  decoder device tests (39 good files pixel-compared frame by frame, exact
  for lossless formats; delays; reset; 41 bad files refused or stopped
  cleanly; the background colour followed; two full passes over the corpus
  leaving PSRAM where it was); the full device suite; release and test
  builds with the configuration guard; a from-scratch configure in the CI
  container.
- Not verified: nothing reaches the panel yet (part 2). libwebp, libpng,
  libjpeg-turbo and AnimatedGIF are run only on the corpus, not fuzzed.
- Noted, not changed:
  - What happens after an animation's last frame differs by format (GIF
    starts again by itself; WebP and APNG report an error until reset), and
    an opaque still gives its frame only once. Both are as in 1.x and are
    now stated in the header. Part 3, with the player's loop in hand,
    decides whether to make them uniform.
  - GIF and APNG report transparency for files that have none. It only
    costs a little time on a background change.
  - The hardware JPEG path repeats colour samples instead of interpolating
    them. Visible only at harsh colour edges; museum photographs have few.

### 2026-11-18: M1.5 parts 3 and 4, setup and the HTTP test hooks. Phase 1 complete

Setup (part 3):

- `net_setup.c` (moved from `wifi_captive_portal.c`, the last file of
  `legacy/components/wifi_manager`): when the state becomes `setup` a job
  turns the radio into the open access point `p3a-setup` at 192.168.4.1,
  starts the DNS catch-all and mDNS, and emits
  `P3A_EVENT_NET_SETUP_STARTED`. The 1.x second HTTP server is gone: the
  one server serves setup too.
- `net_dns.c`, pure: the DNS answer. 1.x built it inline in the task. It
  now refuses anything that is not a plain query, checks that the name ends
  inside the packet, drops what follows the first question (the 1.x reply
  was malformed when the query carried an EDNS record), and answers
  questions for anything but an IPv4 address with "no record", so that a
  phone falls back at once instead of waiting.
- Built-in pages: `tools/embed_static.py` gzips the folder
  `components/p3a_net/static/` into a C table at configure time
  (`http_static.h`); the server serves a GET that no route took from it,
  with an ETag. Adding a page is adding a file. The web UI (M5.1) uses the
  same path. This is the first piece of announced change A (one binary, no
  web UI partition).
- The portal behaviour lives in the server's "no route" path: in setup, an
  unknown GET outside `/api/` is redirected to `http://192.168.4.1/`, and
  `/` is the setup page. On the LAN the same requests are plain 404s.
- One setup page instead of four. `static/setup/index.html` keeps the 1.x
  look and talks to `/api/v2/wifi`; the 1.x success, error and erased pages
  became states of it. 3.2 KB on the wire.
- `GET`, `PUT` and `DELETE /api/v2/wifi`. `PUT` checks the network name,
  the password and the optional device name before storing anything.
- A behaviour change, decided as D35: a stored network that is merely
  absent at boot is retried for two minutes before setup is offered, where
  1.x gave it three tries (about ten seconds) and then sat in setup until
  someone power-cycled it. A unit that boots faster than the router after
  a power cut now comes back by itself. A network that is there and
  refuses the connection still gets three tries, so a mistyped password
  shows the portal again within a minute. The rule is a pure function
  (`net_first_join_gives_up`), host-tested.

HTTP test hooks (part 4), in `p3a_test_http.c`, registered through
`p3a_http_add_routes()` and absent from a release build:

- `GET /api/v2/test/screenshot[?div=N]`: the panel as a BMP. A 90 by 90
  thumbnail takes 0.1 s, where the UART route takes about fifteen. The
  full 720 by 720 frame (1.5 MB) takes about 8 s: the device sends at
  roughly 190 KB/s to the LAN. That rate is a property of the link through
  the co-processor, not of the hook (batching the rows changed nothing),
  and is worth measuring properly when uploads and updates arrive.
- `GET /api/v2/test/log[?since=N]`: the last 256 log lines, numbered, so a
  test can read what a command logged without the UART.
- `GET` and `POST /api/v2/test/clock`: read and set the clock, for the
  tests of everything that will depend on dates.

Numbers from the device: in setup the internal pool has 278 KB free of 413.
The setup page is 9,988 bytes, 3.2 KB gzipped. Release image 988 KB.

- Verified, host tests: the DNS answer (shape, the empty answer, the
  dropped tail, every refusal, every buffer length, garbage of every
  length), the lookup of built-in pages against the real embedded table,
  the give-up rule. Verified, tool self-tests: the embedder (round trip,
  reproducible output, refusals). Verified, device tests: the page over the
  LAN, gzipped, byte for byte the file, with 304; no redirects on the LAN;
  the Wi-Fi endpoints, including `PUT` refusing eight kinds of bad input
  without storing anything and a good `PUT` renaming and restarting;
  `DELETE` leading to setup with the access point up, every name resolving
  to the device, nine operating-system probes and other unknown pages
  redirected, the page at `/`, the API untouched; the network seen in the
  workstation's Wi-Fi scan; a refused password reaching setup in under 80 s;
  an absent network still being tried after 60 s and reaching setup at
  about two minutes with no driver re-initialisation; the HTTP screenshot
  equal to the UART one and exact against known pixels; log capture and
  paging; setting the clock.
- Not verified: a phone actually joining and its sign-in sheet opening
  (manual check M1.5-a); the mDNS alias that lets a named device answer to
  its own name during setup (kept from 1.x). The DNS and HTTP halves were exercised over
  loopback on the device, not over the radio.
- Left for later: the setup screen on the panel (M3.3), which will also
  want the number of joined clients; a list of nearby networks on the
  setup page (recorded in `ideas.md`).

Phase 1 exit ("the device boots, mounts the card, holds settings, joins
Wi-Fi and answers over HTTP", and M1.5's "from erased flash to answering
`/api/v2/status` with no human step") is met: `lab.py up` followed by
`lab.py provision` does it.

### 2026-11-18: M1.5 part 2, the HTTP server and `/api/v2`

- Fab decided Q2 and Q6 (D33, D34): keys always masked with one narrow
  call for the two museum keys; crash dumps downloadable and erasable.
- New in `components/p3a_net`, all new code (the 7,450 lines of
  `legacy/components/http_api` stay in the quarry; their handlers move with
  their features):
  - `http_route.c`, pure: the route table and the one matcher. Literal
    segments, `{param}` segments, a trailing `*`; 404 told apart from 405
    with the allowed methods; URL decoding that refuses malformed escapes
    and control characters.
  - `http_envelope.c`, pure: the success and error envelopes (the error one
    needs no heap), the status and code for every `esp_err_t` including
    each p3a error, weak ETags and `If-None-Match` matching.
  - `http_server.c`: one wildcard registration per method into one
    dispatcher. A handler receives a request object and answers exactly
    once through `p3a_http_ok`, `p3a_http_accepted` or `p3a_http_fail`; the
    envelope, the `Content-Type`, `Cache-Control` and, for a GET, the ETag
    and the 304 are applied there. A handler that does not answer gets a
    500 sent for it. Bodies are capped at 16 KB and must be a JSON object.
    The socket settings of 1.x are kept (12 sockets, LRU purge, reset on
    close, core 0).
  - `net_api.c`: status, settings (read, change, schema), the museum keys,
    jobs, reboot, factory reset, crash dump (read and erase). Other tables
    are added with `p3a_http_add_routes()`.
- The server starts with the network layer and stays up whatever the Wi-Fi
  state, because setup will be served by it too.
- The settings schema is embedded as written and served unwrapped: it is a
  document, not an answer.
- Test hook `crash` (a deliberate `abort()`), and a `expects_crash` marker
  so that one test may crash the firmware without failing the crash watch.
- **A bug found by the new crash test, and a platform rule.** The first
  version read the crash-dump summary in a job. With a dump stored, the
  device crashed on every boot: `assert failed:
  spi_flash_disable_interrupts_caches_and_other_cpu` on task
  `p3a_jobs_io`. The IDF reads the summary by memory-mapping the partition;
  mapping flash turns the cache off for a moment; a task whose stack is in
  PSRAM cannot run with the cache off. Every task class except `RENDER`
  has a PSRAM stack. The dump is now read once at boot from `app_main`'s
  task, and the function refuses to run from a PSRAM stack. The rule is in
  `AGENTS.md`. Ordinary flash reads and writes (`esp_partition_read`, NVS)
  are safe from any task, as the settings tests show; it is the `mmap`
  family that is not. This matters for firmware updates: `esp_ota_end()`
  verifies the image through the same mapping, so an install cannot simply
  be a job (new open question O16).
  The dump endpoint diagnosed this bug itself: read over HTTP, it named the
  task and the assert.
- Numbers from the device: the server costs about 17 KB of the internal
  pool (280 KB free of 416 with Wi-Fi up and the server idle). 120 status
  requests in a row lose no memory and drop no connection. Release image
  now 977 KB (Wi-Fi, mDNS and the HTTP server linked in; it was 757 KB
  after M1.4).
- Verified, host tests: 19 cases for the matcher, the decoder, the
  envelopes (every truncation length parses as JSON), the error map and the
  ETags. Verified, device tests over the real network, cross-checked on the
  UART: status; 404, 405 with `Allow`, 414; ETag and 304, and a new ETag
  after a change; settings equal to the store; PATCH saving, all or
  nothing, with the reason; non-object, truncated, empty and oversized
  bodies refused; all four keys masked, the museum call returning two with
  `no-store` and no ETag; the schema byte for byte; jobs by id; reboot;
  factory reset needing its confirmation, then erasing and landing in
  `setup`; a crash producing a dump that is summarised, downloaded (an ELF
  of the reported size), erased, and gone after a restart; 120 requests
  leaving no loss.
- Not verified: behaviour with more than 12 simultaneous clients, and slow
  or stalled clients (the 10 s timeouts). Candidates for the soak work.
- Deferred: `uptime_s` makes the status answer different every second, so
  its ETag never matches; the web UI's polling design (M5.1) decides
  whether status is split into a stable and a live part.

### 2026-11-18: M1.5 part 1, the device is on Wi-Fi

- New component `components/p3a_net` (layer `net`). Moved out of
  `legacy/components/wifi_manager` with `git mv`: `app_wifi.c`
  (`net_wifi.c`), `wifi_recovery.c` (`net_wifi_recovery.c`),
  `transport_recovery.c` (`net_link_recovery.c`), `sntp_sync.c`
  (`net_time.c`), and the two headers. Only `wifi_captive_portal.c` is left
  there, for part 3.
- What changed on the way in:
  - `p3a_net_start()` never waits. 1.x blocked boot for up to 30 s and then
    fell into the portal; 2.0 reports through `P3A_EVENT_NET_UP` and
    `P3A_EVENT_NET_DOWN` (emitted on the transitions only) and a state:
    `setup`, `connecting`, `up`, `degraded`.
  - The upward calls are gone: Makapix, the HTTP API, `p3a_state` and the
    weak UI symbols. Whoever cares subscribes to the two events.
  - Nothing aborts. The 1.x `ESP_ERROR_CHECK`s on the Wi-Fi driver calls are
    returned errors.
  - The two long-lived tasks come from `p3a_task_create()`; the restart
    countdown is a task of class `APP` made only when needed.
  - The three restart counters are `p3a_recovery` (WIFI: one restart; LINK:
    three), cleared by the first address.
  - The Wi-Fi build options of 1.x (`ESP_MAXIMUM_RETRY`, the WPA3 and
    auth-mode choices, the AP name) are constants with the values 1.x
    shipped: three initial tries, WPA2 floor, WPA3 with both key methods.
  - The reconnect delay is a pure function (`net_backoff.h`), host-tested,
    and can no longer overflow.
  - Credentials keep the 1.x namespace and keys. They are validated before
    being stored, the password buffer is wiped after use, and neither is
    ever logged.
  - The hostname comes from the `device_name` setting: `p3a` or
    `p3a-<name>`.
- `p3a_clock` (util): whether the clock holds real time, and ISO 8601
  formatting with no dependence on the C library's time zone. It lives in
  `util` because sources (below `net`) must ask before trusting a date;
  `net_time.c` marks it set. 1.x kept this inside the Wi-Fi component and
  nine source files reached up for it.
- Preserved on purpose (and stated at the top of `net_link_recovery.c`):
  the restart ladder never depends on the co-processor's firmware version;
  no in-place re-initialisation of esp_hosted (it kills the SD card); a
  second `CP_INIT` is logged, not acted on.
- Test hooks `net` and `clock`. The lab gained `provision` (credentials
  from the secrets file over the UART, never in a binary, redacted from
  logs) and `wait_online`; the device-test session ends by leaving the unit
  online.
- The dev unit's name. The first run put the unit on the LAN as plain
  `p3a`, the name of another device on this LAN (a friend's unit), for about ten minutes before
  it was noticed. The lab now always names the dev unit `fab`
  (`DEV_UNIT_NAME`), sets the name before the credentials, and no test
  joins under the bare name.
- One existing test changed: the allocation-failure test exhausts the
  internal pool, and with the Wi-Fi station sharing that pool its snapshot
  assertions became a race (seen once: `largest` exactly 2048). It now runs
  with the station off and restores Wi-Fi afterwards. What exhaustion does
  to a live network belongs to the soak work (M6.1).
- Numbers from the device, Wi-Fi up and idle: internal DMA pool 417 KB
  total, 297 KB free, lowest 286 KB (before Wi-Fi: 425 and 323). Boot to
  address about 10.5 s, of which 4.7 s is the co-processor link coming up;
  SNTP answers within 100 ms of the address. Signal at the bench -45 dBm.
- Verified, host tests: the clock formatter against values from Python
  (leap days, 2038, 2100, before 1970); the backoff schedule and its cap.
  Verified, device tests on the real network: boot does not wait; joins and
  reports address, signal and name; the password never printed; one up
  event per connection; DNS resolves; the clock within 10 s of the
  workstation's and no 1970 timestamps before; the unit reachable at its
  address; rejoining after a forced drop with one down and one up event and
  no re-initialisation; `device_name` becoming the hostname; mDNS
  resolving from the workstation; bad credentials refused; nothing stored
  ending in `setup`; a network that is not there ending in `setup`.
- Not verified: the driver re-initialisation, the restart ladders and the
  degraded state. They need a failing router or co-processor; the code
  moved with its logic intact and its integration points replaced. They
  are candidates for fault injection in the soak work (M6.1). The restart
  countdown shows nothing on screen until the UI exists (M3).
- Known gap until part 3: a device in `setup` stays there, logging that it
  needs setting up. The portal plugs into that state.

### 2026-11-18: M1.4 settings complete

- Fab decided four questions first (D27 to D30): brightness is remembered,
  a factory reset exists in the API and the web UI, the five API-only
  settings stay settings but hidden from the page, and the keys are renamed
  to one convention.
- `components/p3a_settings/settings.json` is the source of truth: 34
  settings, each with type, default, limits, group, label and the flags
  `reboot`, `advanced`, `secret`. `tools/gen_settings.py` checks it and
  generates the C table and the typed struct `p3a_settings_t` (512 bytes) at
  configure time, for the firmware and for the host tests alike. A bad entry
  stops the build with a message that names it.
- New component `p3a_settings` (layer `storage`):
  - `p3a_settings_model.c`, pure: defaults, validation, JSON in and out. A
    request is applied all or nothing; an unknown key is refused by name;
    secrets read back masked, and the mask written back keeps the secret.
  - `p3a_settings.c`: the store. NVS is read once at boot; reads are a
    struct copy under a mutex; a change writes the non-default values as one
    JSON blob (`{}` on a fresh device) and emits one
    `P3A_EVENT_SETTING_CHANGED` per changed setting, never logging a value.
    Stored entries this firmware does not accept are skipped at load; a blob
    that does not parse is kept aside under another key and the defaults are
    used (D31).
  - Factory reset erases the NVS partition (D28).
  - `p3a_recovery`: the counted restart-and-retry rule that 1.x had three
    copies of (Wi-Fi, touch, co-processor link), as one helper with a streak
    and a total per kind. A failure whose restart cannot be counted is
    answered with "carry on", never with an unbounded loop.
- Boot now reads the settings first: the panel comes up in the stored
  background colour and brightness, the SD root comes from `sd_root`, and a
  touch controller that fails restarts the device once before carrying on
  without touch (the 1.x recovery, deferred from M1.2).
- Nothing was moved out of `legacy/config_store`: the store is new code, and
  the 1.x one (2,266 lines, 95 functions, one cached getter and setter per
  key) stays in the quarry until its last readers have moved. See the
  deletion log entry for what was deliberately not carried over.
- cJSON 1.7.19 (the release the IDF ships) is vendored under
  `tests/host/third_party/` so that JSON code is host-testable.
- CI runs the generator self-test.
- Checked against 1.x: every default and limit in the schema was compared
  with `legacy/components/config_store` and the 1.x Kconfig defaults. One
  first-draft value was wrong (Makapix refresh minimum) and was corrected
  before the checkpoint.
- Numbers from the device: internal DMA pool 425 KB total, 323 KB free
  (was 427 and 326 before the store); release image 757 KB (was 721).
- Verified, host tests: the model test runs against the real schema and
  covers every type's limits, the three named checks, masking, all-or-nothing,
  a hostile key, and a store-and-reload round trip of all 34 settings moved
  off their defaults. Verified, device tests: defaults on a fresh device;
  eight settings across a reboot; brightness applied at boot; refused
  requests changing nothing; one event per changed setting; the typed
  setters; the reboot flag; a secret never shown or printed; `sd_root`
  moving the root at the next boot; skipped entries; an unparseable blob
  kept aside; factory reset; recovery counters across reboots.
- Not verified: the touch-recovery restart end to end, which needs a
  controller that fails. NVS wear from frequent writes is not measured; a
  caller fed by a slider must settle before it writes (stated in the header,
  to be honoured by the API at M1.5 and the UI at M5.1).
- Left for later, on purpose: nothing subscribes to the change event yet, so
  a brightness or rotation change made at run time is saved but takes effect
  at the next boot until the screen owner exists (M3.1). The web UI reads
  the schema file at M5.1.

### 2026-11-18: CI repaired, M1.3 storage complete

CI:

- The firmware jobs had been failing since the co-processor commit with
  "Missing required kconfig option after retry". Reproduced by running the
  CI container locally (Docker inside WSL) on a clean clone; a from-scratch
  build on the workstation had passed, which is why it was not seen before
  pushing.
- Cause: without a warm cache the component manager also reads the
  manifests of esp_hosted releases newer than the pinned 2.9.7. Those make
  three USB host libraries conditional on `ESP_HOSTED_HOST_USB_*_MANIFEST
  == 1`, symbols that no Kconfig file defined, because the pinned release
  predates them. The first theory in the pause note (transitive pins needing
  more than one retry) was wrong, and so were two more tried on the way: the
  component manager version (2.4.8 in the container, 2.4.10 on the
  workstation; upgrading changed nothing) and a stale lock file (the
  container does not rewrite it).
- Fix (`64858edc`): `components/slave_ota/Kconfig` defines the three as
  hidden integers equal to 0. A boolean is rejected by the resolver (the
  rules compare with 1), and a promptless boolean that is off is not even
  written to the generated configuration; both were tried first.
- New routine, in `AGENTS.md`: a dependency change gets a from-scratch
  configure in the CI container before it is pushed.

M1.3:

- New component `components/p3a_storage` (layer `storage`). `sd_path`,
  `fs_atomic`, `sd_health` and `sd_repair` moved out of
  `legacy/components/p3a_core`.
- `sd_path` is split: `sd_path.c` is pure (root, sanitizing, shard layout)
  and host-tested; `sd_path_fs.c` creates folders. `sd_path_init()` takes
  the configured root as an argument instead of reading the 1.x config
  store. A root of `"/sdcard/"` alone, which 1.x accepted and which
  produced paths with a double slash, is rejected.
- `sd_health` announces a failure on the new event bus
  (`P3A_EVENT_SD_HEALTH_FAILED`, a real fan-out: downloads, the UI and the
  API all need to know). `fs_atomic` answers `P3A_ERR_SD_FAILED` on a
  failed card instead of `ESP_ERR_INVALID_STATE`. `sd_repair` allocates
  from PSRAM. All three use the board's mount-point constant.
- New, `p3a_crc32` in `util`: one CRC-32 for device and host.
- New, `p3a_frame` (decision D25, review finding 5): the one container for
  binary files, a 32-byte header and fixed-size records, with a schema
  number, a version, flags and two checksums. The pure half checks a file
  image; the file half saves through `fs_atomic` and loads into PSRAM. A
  failed check tells damage (`CORRUPT`, or `ESP_ERR_INVALID_SIZE` for a
  truncated file) from another firmware's intact file
  (`ESP_ERR_INVALID_VERSION`) from no file (`MISSING`), and never deletes
  or overwrites anything.
- New, `p3a_storage.h`: `p3a_storage_start()` is the boot sequence in the
  one safe order (mount, FAT mirror check and remount if repaired, root,
  folders, probe), and the header states once what p3a assumes about FAT.
  Free space is measured by a job after boot and read from a cache
  (`p3a_storage_space()`); nothing computes it in a request path.
- `app_main` calls `p3a_storage_start(NULL)`; the root comes from settings
  at M1.4.
- Test hook `storage`.
- Numbers from the device: the FAT mirror check takes about 1.0 s at boot
  (14,903 sectors per copy, none differing). A 2,048-record container (33 KB)
  saves in about 80 ms and loads in about 16 ms. The card holds 29,805 MB
  with 27,744 MB free. Internal DMA pool unchanged at 427 KB total, 326 KB
  free.
- Verified, host tests: 8 programs. The container test flips every single
  bit of a file image and requires each flip to be caught; the shard layout
  is checked against values computed by an independent Python
  implementation. Verified, device tests: 62 pass, twice. They cover boot
  bringing storage into service, the cached free-space figure, atomic
  replace with no temporary file left, container round trips of 0, 1 and
  2,048 records with a flipped byte, a truncation and a version mismatch
  (the file still there each time), the shard layout against the same
  Python implementation, and the health latch: a disk-full error does not
  trip it, three I/O errors do, it is announced once, writes are then
  refused with `SD_FAILED`, and only a reboot clears it. Verified, builds:
  release (721 KB) and test, with the configuration guard.
- Not verified: `sd_repair`'s repair paths. They need a card that damages
  itself, and the dev unit's card is healthy; the code moved with only its
  allocator and one constant changed. The boot-time FAT comparison runs and
  finds nothing to do.
- Left open: the dev unit's card still holds 1.x folders (`/p3a2`, and
  whatever 1.x left under `/p3a`, which is also 2.0's default root). They
  do not interfere yet. Decide at M2.2, when 2.0's own layout exists,
  whether the lab wipes the root before a test session.

### 2026-11-17: M1.2 complete, the co-processor link

- CI on the SD commit: see `gh run list --branch v2`; the run for this
  commit covers both.
- `components/slave_ota` moved (layer `board`). It declares esp_hosted
  2.9.7 and esp_wifi_remote 1.2.2 with their dependencies, pinned to what
  1.x shipped. The three `ESP_HOSTED` lines in `sdkconfig.defaults` are
  live.
- The configuration guard did its job on the first build with esp_hosted:
  2,368 settings are now defined by both configurations and every one
  agrees, so the SDIO transport, the C6 target, streaming receive mode and
  the pins are exactly what 1.x shipped. It also failed the build until the
  three Wi-Fi-stack entries were removed from its expected list. Three
  expected differences remain, none about hardware.
- `slave_ota` no longer includes `p3a_state.h` or `p3a_render.h`, which cut
  the `p3a_core`, board, `slave_ota` cycle the review lists. An update shows
  itself through three callbacks (`slave_ota_set_ui`) that the UI will
  register in Phase 3; without them it runs unseen and logs.
- `slave_ota_check_and_update()` now brings the SDIO link up itself
  (`esp_hosted_connect_to_slave`). In 1.x the Wi-Fi driver had done that
  first; in 2.0 the check runs before Wi-Fi exists.
- Cleanups on the way: the transfer buffer comes from PSRAM, an image whose
  header claims more than the partition holds is refused (a low-severity
  audit item), and a missing `network_adapter.bin` fails the build instead
  of warning.
- `app_main` runs the check at boot. A failure is logged and boot carries
  on.
- Test hook `c6`.
- What the dev unit can and cannot show: its C6 runs esp_hosted 2.7.0, the
  one version that cannot be updated (esp-hosted-mcu issue 143). So the
  link, the version exchange, the lock detection and the image in
  `slave_fw` are verified here, and the update transfer is not. That is
  manual check M1.2-e, on another board.
- Numbers: the link is up 1.8 s after `app_main` starts. Internal DMA pool
  with the link up: 427 KB total, 327 KB free (esp_hosted took about
  48 KB), largest block 254 KB. Release image 709 KB.
- Verified, device tests: 47 pass after a flash and again without one.
  Verified, builds: release and test pass the build checks and the
  configuration guard.
- Closed by M1.2 as a whole: audit H5 and M28, and the low-severity items
  about `sdio_bus_is_locked()` and the unclamped slave image size. The
  `p3a_core` to board to `slave_ota` cycle is gone.
- Left in `legacy/` for later milestones: the rest of `p3a_core`, the
  gesture code in `main/app_touch.c` (M3.4), `main/app_lcd_p4.c` (display
  glue around the renderer, M2.1), `main/app_usb.c` (M3.6),
  `main/sd_format.c` (M3.3).

### 2026-11-17: M1.2 third step, SD card and its fixes

- Fab asked to keep going and to use the move as an opportunity to clean up
  what 1.x left behind. Taken as standing guidance: a file that moves is
  cleaned, not ported verbatim.
- CI on the touch commit (`a7f69462`): all four jobs green.
- `p3a_board_sd.c`: mount and unmount through the BSP, never formatting.
  The health checks, the FAT mirror repair and the folder tree stay for the
  storage layer (M1.3), which will call this.
- `p3a_board_ff_alloc.c` (was `legacy/main/ffsystem_aligned.c`): FatFS
  buffers come cache-line aligned from PSRAM, so the SD driver uses them
  directly instead of bouncing every transfer through the internal DMA
  pool. The linker wrap now lives with the board component, not in `main`.
- `components/sd_idle_wait` moved (jitter fix 8, the yielding busy-wait).
  Its detection of Espressif's own fix no longer uses a weak reference: the
  component's CMake reads the IDF source at configure time and passes the
  answer in as a definition (decision D23).
- `components/sdio_bus` moved and rewritten shorter. `sdio_bus_is_locked()`
  reads an atomic holder instead of taking and releasing the mutex (a
  low-severity audit item).
- `components/frame_trace` moved now, as a `util` component, because
  `sd_idle_wait` records its marks through it. It still compiles to nothing
  in release. Its reporter task is created through `p3a_task_create`. Its
  HTTP endpoints remain in `legacy/components/http_api` for M2.1.
- The overlays `sdkconfig.trace.defaults`, `sdkconfig.diag.defaults` and
  `sdkconfig.nowrap.defaults` are back at the root; the lab knows the
  `trace` and `diag` configurations.
- New guard, `tools/compare_sdkconfig.py` (decision D24): every setting that
  both the shipped 1.x `sdkconfig` and the generated 2.0 one define must
  agree, except a short list of expected differences with reasons. First
  result: 2,107 settings in common, 6 differ, all explained (PICO-8's
  WebSocket support, the old web UI mount point, an informational version,
  and three that the Wi-Fi stack switches on and that must leave the list
  at the co-processor step). 436 settings exist only in 1.x: components not
  brought in yet, or dropped. The lab runs it after every release build and
  CI after its release build.
- Test hook `sd` (info, and a write, read-back, delete of a scratch file).
- Numbers from the device: card "SDABC", 29,820 MB. A 1 MB scratch file
  writes in about 200 ms and reads back in about 450 ms. Internal DMA pool
  with the card mounted: 432 KB total, 375 KB free, largest block 254 KB.
- Verified, device tests: 43 pass after a flash and again without one. They
  confirm the card mounts at boot, FatFS buffers are aligned and in PSRAM,
  the yielding busy-wait is linked, the IDF is not patched, and files from
  1 byte to 1 MB round-trip. Verified, builds: release (419 KB) and test
  pass the build checks; the trace configuration compiles and links the
  frame trace; the release map has no frame-trace symbol. Not verified: the
  trace build was not flashed, and nothing here measures jitter (M2.1 and
  M6.1).
- Still on the card: the 1.x folders (`/p3a2`, `/p3a`). M1.3 decides when
  to wipe them.

### 2026-11-17: M1.2 second step, touch controller

- CI on the first board commit (`58ccd894`): all four jobs green. The BSP
  and its dependencies resolve inside the CI container.
- `components/p3a_board/p3a_board_touch.c`: the I2C bus recovery and the
  controller init, moved from 1.x. `p3a_board_touch_init(timeout_ms)` runs
  the blocking init on a task of its own and waits a bounded time. The
  context shared with that task is reference-counted on the heap, which
  fixes audit H5 (1.x kept it on the caller's stack and deleted its
  semaphore while the task could still use it).
- The init task is created with plain `xTaskCreate`, at priority 1, not
  through `p3a_task_create` and not as a job. Both exceptions are deliberate
  and explained at the call: the task ends (the helper is for tasks that do
  not), and it may hang forever (a job would take its runner with it).
- What 1.x did around a touch failure is not here yet: the persisted reboot
  streak, the on-screen countdown, the single retry by reboot, and the
  rescue task that forces a restart when the init starves the main task.
  They need the settings store (M1.4) and the UI (Phase 3). For now a
  failed or timed-out init is logged and the device carries on without
  touch. [PARITY.md](PARITY.md) now assigns that row to M3.4.
- The gesture code in `legacy/main/app_touch.c` stays in `legacy/` for
  M3.4.
- Test hook `touch`: one read of the controller.
- Verified, device tests: 33 pass after a flash and again without one. The
  controller is ready at boot with no failure or timeout message and
  answers 20 reads in a row with no points. Verified, build checks: release
  (321 KB) and test. Awaiting manual check M1.2-d: nothing can put a finger
  on the glass.

### 2026-11-17: M1.2 first step, panel, backlight, button

- CI on the M1.1 commit (`f544925f`): all four jobs green.
- New component `components/p3a_board` (layer `board`).
  `p3a_board_display.c`, `p3a_board_button.c` and `p3a_board.h` moved from
  `legacy/components/p3a_board_ep44b`. The header is rewritten for one
  board and one pixel format: no capability flags, no RGB565 path, no
  redefinition of Kconfig symbols (audit M28).
- The BOOT button no longer emits an event or knows about the co-processor
  update: the board reports a debounced press to one callback, and what a
  press means is the caller's business. One listener, so not an event (D22).
- Dependencies: the Waveshare BSP and everything it pulls in are declared
  in `components/p3a_board/idf_component.yml`, pinned to the exact versions
  1.x shipped with. `dependencies.lock` at the root is tracked. LVGL comes
  with the BSP and is not used; dropping the BSP for the bare panel driver
  would remove it, which is an idea for `ideas.md`, not part of this work.
- `sdkconfig.defaults`: the three BSP display lines are live; the
  esp_hosted lines stay parked for the co-processor step.
- `app_main` brings the display up painted black. The background colour
  will come from the settings store (M1.4).
- Test hooks: `p3a_test_board.c` with `lcd` (info, fill, bars, gradient,
  pixel, crc, shot, brightness) and `button`. `lab.py shot <file>` saves a
  downscaled PNG of the framebuffer over the UART, so the agent can look at
  what was drawn before the HTTP screenshot hook exists (M1.5).
- Found by the tests: a 180x180 screenshot always had one short row. The
  first theory, bytes lost in the host's serial buffer, was wrong. The
  console task printed for more than 15 seconds without yielding, the task
  watchdog reported the starved idle task, and that report landed in the
  middle of a row. The screenshot command now yields once per row. The lab
  changes made while chasing the wrong theory (a 1 MB receive buffer, log
  flushed when idle instead of per line) are kept; they are sound, but
  they were not the fix.
- The agent looked at two screenshots (gradient at 360x360, bars at
  180x180): both correct.
- Memory with the board up: internal DMA pool 434 KB total, 378 KB free,
  largest block 254 KB.
- Image size: release 286 KB, test 367 KB.
- Verified, device tests: 31 pass after a flash and again without one.
  Framebuffer contents are checked by pixel probes and by a CRC of the
  whole frame against a value computed on the host. Verified, build
  checks: release and test. Awaiting manual checks M1.2-a, b and c: the
  tests read the framebuffer, not the glass, and nothing can press the
  button.

### 2026-11-17: M1.1 complete, jobs, events, memory pressure

- CI on the first M1.1 commit (`8d00fe38`): all four jobs green, including
  the repaired test-configuration build.
- Jobs: `p3a_job_table` (pure: ids, states, replacement of the oldest
  finished record) and `p3a_job` (two runner tasks, I/O and UI, class APP,
  12 KB PSRAM stacks, eight queued jobs each). `p3a_job_set_progress()`
  works from inside a job. This is what replaces the twelve one-shot tasks
  of 1.x and what `/api/v2/jobs/{id}` will read at M1.5.
- Events: `p3a_event` with the rule for what the bus is for written at the
  top of its header (decision D22, closes O8). Handlers run with no lock
  held (audit M9); a full queue drops, counts and logs. Subscriber table is
  pure and host-tested.
- Memory: `p3a_mem`. The level decision (OK, LOW, CRITICAL, with a recovery
  margin against flapping) is pure and host-tested; the device half samples
  the internal DMA pool on demand, at most every 250 ms, and announces a
  change as an event. No task and no timer. Thresholds are provisional
  (O9). The allocation-failure heap report moved in from
  `legacy/main/p3a_main.c`, still one per boot.
- `app_main` gained `boot_step()`: a failed step is logged and boot carries
  on (audit H10's direction).
- Test console split into a core and one command file per area
  (`p3a_test_util.c`), with commands `task`, `errname`, `job`, `jobinfo`,
  `event`, `mem`.
- Found by the tests:
  - Host: the recovery margin was computed in 64 bits and then truncated to
    32. Unreachable with real thresholds, fixed anyway.
  - Device: a flash write (NVS set and commit) from a job runner whose stack
    is in PSRAM works on this chip. This was a real question; the runners
    would otherwise have needed internal-RAM stacks.
  - Device: with the near-empty 2.0 firmware the internal DMA pool is
    447 KB with about 400 KB free and a largest block of 254 KB. Fully
    loaded 1.x left about 272 KB. Watch this number shrink as components
    arrive; it is the budget the known SDIO issue is about.
  - Device: exhausting the pool with 2 KB chunks leaves 15 KB free with a
    largest block of 1.4 KB. That is the fragmentation case the "largest
    block" threshold exists for.
  - Two failures were wrong assumptions in the tests themselves (pool size,
    a hog limit in the test hook), not firmware faults.
- Verified, host tests: 6 programs (version, err, task policy, job table,
  event table, mem pressure) pass with MinGW and under ASan and UBSan.
  Verified, device tests: 21 pass, twice in a row, including job ordering
  across runners, a full job queue, event fan-out of 200 through a 32-slot
  queue, a deliberate event flood with drops reported, pressure levels
  following a hogged pool with one event per change, and exactly one heap
  report per boot. Verified, build checks: release and test.
- Not done: `p3a_limits.h`. It has nothing to hold yet; the milestone that
  needs the first shared constant creates it.

### 2026-11-17: M1.1 first slice, allocation, errors, tasks

- CI on the M0.4 commit: tree checks, host tests and the release build
  green; the new test-configuration job failed on an unquoted `;` in its
  own arguments. Fixed in this commit.
- `psram_alloc.h` moved unchanged from `legacy/components/channel_manager`
  to `components/p3a_util`. It keeps its name and API so that code moving
  out of `legacy/` later needs no edits for it. No host test: it is three
  inline wrappers over `heap_caps_*`.
- `p3a_err`: the error base at `0x50000` with `NOT_PLAYABLE`, `CORRUPT`,
  `MISSING`, `RATE_LIMITED`, `AUTH`, `NO_KEY`, `SD_FAILED`, stable names,
  and `p3a_err_to_name()` for any `esp_err_t`. A code added without a name
  fails to compile. First host stub: `tests/host/stubs/esp_err.h`.
- `p3a_task_policy` (pure) and `p3a_task` (FreeRTOS): five classes carrying
  priority, core and stack memory; `p3a_task_create()` is the only way to
  make a task. It uses the IDF's `xTaskCreatePinnedToCoreWithCaps` for
  PSRAM stacks, with a fallback to internal RAM. Tasks made this way are
  long-lived by rule; run-once work will go to the job runners.
- Policy choices, taken from what 1.x shipped rather than from the review
  text: render 5 on core 1 with internal stacks; touch 5, unpinned, PSRAM
  stack (the review says internal, but 1.x deliberately put the touch stack
  in PSRAM to spare the DMA pool); app 4, network 3, background 2, all on
  core 0 with PSRAM stacks. A host test pins these values so that changing
  one is a visible decision.
- The test console now creates its task through the helper and gained two
  commands, `task` and `errname`.
- Verified, host tests: 3 executables (version, err, task policy) pass with
  MinGW and under ASan and UBSan in WSL. Verified, device tests: 8 pass
  after a test build and flash; the console task reports priority 2, core
  0, stack in PSRAM, and error names resolve on the device. Verified, build
  checks: release and test builds pass `check_tree.py --build`.

### 2026-11-17: M0.3 and M0.4, CI and the device lab

M0.3:

- `tools/check_tree.py`: the structural rules as a script (layer rule from
  `components/LAYERS.txt` and each `REQUIRES`, no cycles, nothing requires
  `main`, no weak symbols, no `WHOLE_ARCHIVE`, no includes from `main/` or
  `legacy/`, the guard lines in `sdkconfig.defaults`; with `--build`, the
  generated revision setting, image at most 90 percent of the slot, test
  hooks only in a test build). Same-layer requirements are allowed; cycles
  are not.
- `tools/test_check_tree.py`: 29 cases, one or more per rule, each building
  a small tree that breaks the rule and expecting the failure. This is how
  "each check can go red" was shown; no broken commit was pushed to `v2`.
- `.github/workflows/v2.yml`: tree checks, lab unit tests, host tests under
  ASan and UBSan, firmware builds for release and test in
  `espressif/idf:v5.5.4`, binaries kept 14 days.
- Verified, CI: the first run (`f917b4a6`, release build only) was green on
  all three jobs. This checkpoint adds the test-configuration build and the
  lab unit tests to CI; check `gh run list --branch v2` for that run.

M0.4:

- `components/p3a_test_hooks` (layer `net`, compiled only with
  `CONFIG_P3A_TEST_HOOKS`): a UART console without echo. Protocol in its
  header: `!command [args]` in, `TEST|ok|json`, `TEST|err|message` or
  `TEST|ready|json` out. Commands: `version`, `reboot`.
- `sdkconfig.test.defaults` and the `build-test/` directory.
- `host/p3a-lab/lab.py`: build with checks, flash, erase, a UART class that
  attaches without resetting, logs every line, redacts secrets, detects
  crashes, and speaks the console protocol; `bring_up()` refuses a board
  that is not the dev unit or runs another version. `README.md` beside it.
- `tests/device/`: fixtures (session bring-up, per-test crash watch,
  `--no-flash`, the `live` marker) and six smoke tests.
- Changed from the plan: the `provision` console command is not here. There
  is nowhere to store credentials until the settings store (M1.4) and Wi-Fi
  (M1.5) exist. The lab half (secrets loading and redaction) is done.
- Found by the tests: the first command after a reset was lost once, in the
  first full run. Most likely a stray byte left in the device receive buffer
  by the reset, glued to the front of the line. The lab now sends an empty
  line after the ready message. Four clean runs since; the cause is not
  proven.
- Found while building the lab: `idf.py` refuses to run under an MSYS
  environment (Git Bash) and still exits 0, so a wrapper sees a "successful"
  build with no output. The lab strips the `MSYSTEM` variables.
- Verified, device tests: 6 pass after a full test build and flash, and 3
  more times with `--no-flash`. Verified, build check: the release map has
  no `p3a_test_` symbol; the test map has 53.
- Design point for M2.2, noted while writing the layer table: `sources` sits
  below `library`, yet both need `record_t` and `source_ops_t`. Those types
  need a home at or below `sources`; decide where when M2.2 defines them.

### 2026-11-17: M0.2 host test harness

- `tests/host/`: CMake project, Unity 2.x vendored unmodified from the IDF
  tree, `p3a_host_test()` helper, warnings as errors, sanitizers on by
  default off Windows. Conventions in `tests/host/README.md`.
- First 2.0 component, `components/p3a_util`, with its first unit:
  `p3a_version` (parse and order `MAJOR.MINOR.PATCH[-suffix]`). It is the
  real code that proves the wiring, and OTA needs it at M5.2. Unlike the 1.x
  comparison, a string that fails to parse has no version instead of
  comparing as equal, and a release orders above its own pre-releases.
- No FreeRTOS stub yet: nothing needs one. `tests/host/stubs/` is created
  with the first stand-in, most likely at M1.1.
- Installed `cmake` and `ninja-build` in the WSL Ubuntu so the sanitizer run
  works locally.
- Verified, host tests: 7 cases pass with MinGW and in WSL under ASan and
  UBSan. Verified, local release build: firmware builds with `p3a_util`.
  Not flashed; the app does not use the unit yet.

### 2026-11-17: M0.1 tree reset and empty app

- Fab filled in the Wi-Fi password, approved pushing `main` (pushed), and
  gave the go.
- Moved the 1.x tree to `legacy/` with `git mv` (`components/`, `main/`,
  `webui/`, the root build files, the sdkconfig files, `flasher/`), and the
  untracked `managed_components/` with it. Deleted what never moves; see the
  deletion log.
- New root project: `CMakeLists.txt` (version `2.0.0-dev`), `main/p3a_main.c`
  (prints version and chip revision), the D1 `partitions.csv`,
  `sdkconfig.defaults`.
- Decision D21 (closes O11): the configuration is `sdkconfig.defaults`, and
  `sdkconfig` is generated and untracked. Found while doing it: a tracked
  full `sdkconfig` in a tree without esp_hosted would silently lose the
  esp_hosted and BSP settings on its first regeneration, and they would come
  back as defaults at M1.2. `idf.py save-defconfig` on the 1.x project
  reduced its configuration to 60 lines; the 2.0 file keeps the IDF-level
  ones and parks the BSP, esp_hosted and TinyUSB ones, commented, under
  their milestones.
- Verified, local release build: builds with no warnings; image 226 KB in a
  10 MB slot; the generated `sdkconfig` has `ESP32P4_REV_MIN_FULL=1`.
- Verified, on the dev unit: erased the flash, flashed, reset, read the UART
  log. The bootloader lists the eight 2.x partitions at the D1 offsets and
  finds 32 MB of PSRAM; the app prints `p3a 2.0.0-dev` and
  `chip rev v1.0, 2 cores`. No errors in the log.
- `AGENTS.md` rewritten for the new tree; `legacy/README.md` added;
  `host/intro-anim-lab/build.ps1` repointed at `legacy/` (not rebuilt).
- Facts for later milestones:
  - The shipped 1.x configuration set `P3A_RENDER_TASK_PRIORITY` and
    `P3A_TOUCH_TASK_PRIORITY` to 5, not the Kconfig defaults of 8 and 6 that
    the review quotes. M1.1 takes the task classes' priorities from what
    shipped.
  - `HTTPD_WS_SUPPORT` was only for PICO-8 and is not carried.
  - The repository is reachable through two paths on this workstation, and
    a build directory configured through one refuses the other. The old
    `build/` had been configured through the non-Dropbox path and was
    deleted.
  - `host/jitter-lab/build.ps1` now builds the 2.0 app and its guards will
    fail; `host/p3a-lab/` replaces it at M0.4 and the lab is adapted at M2.1.

### 2026-11-17: foundations

- Studied the architecture review, the user documents, the lab tooling and
  the dev unit's live status.
- Collected Fab's decisions D2 to D13 in three rounds of questions; recorded
  them with the earlier constraints, the working decisions and the open
  ones in [DECISIONS.md](DECISIONS.md).
- On `main` (`4c9de368`): removed the rule against building, flashing and
  monitoring unasked from `AGENTS.md` and `CLAUDE.md` (D5), and recorded the
  branch policy there.
- Created the `v2` branch and this folder: README, PLAN, PROGRESS,
  DECISIONS, TESTING, PARITY. Pointed `AGENTS.md` on `v2` at it.
- Created `%USERPROFILE%\.p3a-lab\secrets.json` with the four API keys read
  from the dev unit (values never printed).
- Took a full flash backup of the dev unit with esptool: 33,554,432 bytes,
  SHA-256 `e6f393e371d06f40993c209d3964e847b322b9fe752aec53caf2d90f69094363`.
  Its partition table parses as the 1.x layout and `ota_0` starts with a
  valid image header. The unit rebooted into 1.2.4 afterwards and answers on
  the LAN. The read took 9 minutes at 921600 baud.
- Checked the toolchains: ESP-IDF 5.5.4 activates and `idf.py` runs; MinGW
  gcc, CMake, Ninja, CTest, Python with pytest, pyserial, requests and
  Pillow, Node and WSL are present. No firmware build was run.
- Built [PARITY.md](PARITY.md) from the user documents plus a read-only
  agent sweep of the 1.x code. The sweep is unverified line by line; rows
  are re-checked as their code moves. It surfaced ten behaviours to decide
  (Q1 to Q10).
- Finding worth keeping in view: a 1.x unit installs the newest release that
  has an asset named `p3a.bin`, comparing versions only
  (`components/ota_manager/github_ota.c`). This is why D12 exists.
- Nothing was built or flashed. No 2.0 code exists yet.
