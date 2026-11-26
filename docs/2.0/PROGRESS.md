# 2.0 progress ledger

Part of the [2.0 work stream](README.md). This is the first page to read in
any session and the last to update in any checkpoint. Keep "Now" true at all
times.

## Now

| | |
|---|---|
| Phase | 4, remote sources (Phases 0 to 3 complete but for their manual checks) |
| Current milestone | Phase 6: M6.1 (soaks) and M6.2 (parity audit) together. Phase 5 is done but for what waits on Fab (M4.5-a, M5.2-a, M5.3-a, M5.3-b) |
| Next action | Phase 6's machine work is done as far as it goes without Fab: the jitter and memory soaks passed (RUN-20261125-02, RUN-20261126-01), the parity audit is in `PARITY.md`, `legacy/` holds only what M4.5 part 4 reads. The dev unit has the test build, provisioned, playing the card. Next, in order of use: (1) if Fab has registered the unit (M4.5-a), M4.5 part 4: live Makapix tests, the "Followed" pill, the server's playsets by name, `get_post`, then delete `legacy/components/makapix` and the rest of `legacy/` but the 1.x `sdkconfig`. (2) O21 if Fab answers it (large artworks skipped when memory is short at a swap). (3) Click what was never clicked: upload on the home page, the Makapix registration on the settings page, drag to reorder, Harvard and the Smithsonian in the museum browser. Chrome is connected; stub `window.confirm` before clicking a button that asks. (4) The intermittent failure of `test_giphy.py::test_a_swipe_up_on_a_gif_is_told_to_giphy` (the swipe down did not unpin, once in a whole suite), and `test_the_setup_network_is_on_the_air` once back on the former LAN. (5) A multi-day soak before release (tell Fab first: it occupies the dev unit): `lab.py flash trace`, `python host/jitter-lab/soak.py --hours 72`. M6.3, the field test, is Fab's |
| Blocked on | M4.5 part 4 only: M4.5-a (Fab registers the dev unit with Makapix Club). Nothing else waits on it |
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
| M2.1 Display pipeline and decoders | done 2026-11-19; manual check M2.1-a cleared 2026-11-23. Tiers: host tests (15 programs), the whole device suite (235 cases on the test build; 6 timing cases on the trace-test build), release, test, trace and trace-test builds, the configuration guard. Timing has a baseline of minutes only; the jitter pass bar waits for the soak (M6.1) |
| M2.2 Library core | done 2026-11-19. Tiers: host tests (15 programs), the whole device suite (240 cases), release, test and trace builds, the configuration guard. The `refs` table of the plan was not built (D48) |
| M2.3 Source interface, local source, scheduler | done 2026-11-23. Tiers: host tests (18 programs, under the sanitizers too), the whole device suite (255 cases; one needed its own bound updated for the larger JSON body limit), release, test and trace builds, the configuration guard. Left for later milestones and noted in the entries: uploads do not play at once (M2.4), more than 16 channels (M4.x), weighted shares on the device (needs a second source) |
| M2.4 Loader pipeline, C and D | done 2026-11-23. Tiers: host tests (20 programs, under the sanitizers too), the whole device suite (260 cases pass; the 6 timing cases need the trace build and were not run), release, test and trace builds, the configuration guard. Not seen on the device because nothing produces them yet: deleting a damaged blob (M4.1), the aspect filter (M4.3). Manual check M2.4-a pending |
| M3.1 Screen model and hook table | done 2026-11-23. Tiers: host tests (22 programs, under the sanitizers too), device tests (smoke, UI, pipeline and player: 48 cases), test build. Nothing is drawn yet |
| M3.2 Text and fonts | done 2026-11-23: the font is Everyday Ample, Fab's choice (D61). Tiers: host tests, device tests (the panel equals the workstation's rendering pixel for pixel). Manual check M3.2-a (the look on the panel itself) pending; image assets arrive with the screens that need them |
| M3.3 Screens | done 2026-11-23 except the registration code (M4.5) and the USB screen (M3.6). Tiers: host tests (27 programs, under the sanitizers too), the whole device suite (278 cases passed and 6 timing cases were skipped; 3 failed in the touch tests, whose expectation of a tap on the information screen was out of date, and pass since), including a real format of the dev unit's card. Manual check M3.3-a pending |
| M3.4 Touch router and gestures | built 2026-11-23; open for manual check M3.4-a (real fingers), swipe actions (Phase 4) and the SD-format taps (M3.3). Tiers: host tests (26 programs, under the sanitizers too), device tests with scripted fingers (71 cases with the areas around it) |
| M3.5 Overlays | done 2026-11-23 except source branding, which moves to M4.2 where its sources and their terms are (O6). Tiers: host tests (29 programs, under the sanitizers too), device tests (the overlays equal the workstation's rendering). Manual check M3.5-a pending |
| M3.6 USB mass storage | built 2026-11-23; open for manual check M3.6-a (a real computer). Tiers: host tests (30 programs, under the sanitizers too), device tests with the bus's reports injected (with it every test not marked slow passes: 267 cases), release, test and trace builds, the configuration guard, a from-scratch configure in the CI container |
| M4.1 Fetch, download, refresh, eviction | done 2026-11-25. Tiers: host tests (32 programs), the whole device suite on the M4.1 commit (297 pass, 6 skipped), CI green, release and trace builds (made on the tree with M4.2 part 1 in it). Manual: none. Not verified: listed in the log entry of 2026-11-23; O9's measurements moved to M4.2 |
| M4.2 Giphy and Klipy | done 2026-11-25. Tiers: host tests (35 programs), device tests (the whole suite: 309 passed, 6 skipped), CI, test, release and trace builds. Manual: M4.2-a. Not verified: listed in the log entries of 2026-11-25. O6 (the services' own marks) stays open and holds nothing |
| M4.3 Museums | done 2026-11-25. Tiers: host tests (36 programs, also under the sanitizers), device tests (the whole suite on the last build: 322 passed, 6 skipped), CI, test, release and trace builds. Manual: none. Not verified: listed in the log entries of 2026-11-25. O20 (what p3a tells the museums it is) stays open and holds nothing |
| M4.4 Pinned lists and "show this" | done 2026-11-25. Tiers: host tests (38 programs, also under the sanitizers), device tests (the whole suite: 330 passed, 6 skipped; the pin tests again on the last build), test, release and trace builds. Manual: M4.4-a. Not verified: listed in the log entry |
| M4.5 Makapix | in progress. Parts 1 to 3 (2026-11-25): the source, registration up to the owner's part, the link, commands, views, likes, renewal. Host and device tests for everything an unregistered unit can do. Waits on M4.5-a for the rest |
| M5.1 Embedded web UI | done 2026-11-25 but for the "Followed" pill (M4.5 part 4). Four pages, the museum browser. Tiers: node (42 cases), device tests, every page clicked in Chrome on the dev unit. `legacy/webui` is gone |
| M5.2 Firmware OTA for 2.x | done 2026-11-25 but for manual check M5.2-a (one update from GitHub itself). Tiers: host tests, device tests (the whole cycle from a mirror), and the cycle clicked through the update page in Chrome |
| M5.3 Release engineering | done 2026-11-25 but for manual checks M5.3-a (the web flasher) and M5.3-b (the flasher program). The release folder from `tools/make_release.py`, flashed onto the dev unit with its own command. Tiers: self-tests in CI, one flash by hand |
| M5.4 Documentation | done 2026-11-25. The user's documents and the developer pages describe 2.0; the API examples were run against the dev unit |
| M6.1 Soaks | not started |
| M6.2 Parity audit | not started |
| M6.3 Field test | not started |
| M6.4 Release | not started |

## Manual checklist

Checks only Fab can do (D10). Each line: id (milestone plus a letter), what
to do, what should happen. Fab ticks a line or reports what he saw; a failed
check reopens its milestone.

Open:

- **M5.3-a, the web flasher end to end.** From the page (served locally
  or from Pages once `v2` is `main`), flash a 2.x release folder onto a
  board: the warning shows, the button stays off until the box is
  ticked, the board comes up fresh.
- **M5.3-b, the flasher program.** Build it
  (`python host/p3a-flasher/build_flasher.py release/v<version>`), run
  `p3a-flasher.exe`, flash a board, see the question before it erases.
- **M5.2-a, one real update from GitHub.** Publish a pre-release with a
  `p3a-v2.bin` built from `v2`, turn on "Offer pre-release firmware", look,
  install, and see the device restart into it and be kept. The cycle is
  device-tested against a mirror on the LAN; this adds GitHub's redirect
  and its TLS.
- **M4.5-a, registering the dev unit with Makapix Club** (D13). Needed
  before anything a registered player does can be tested (part 3 on).
  With the unit on and on the network: `curl -X POST
  http://p3a-fab.local/api/v2/makapix/registration`. A six-character code
  appears on the screen, large, with "sign in at makapix.club and enter
  this code" under it. Type it at makapix.club within 15 minutes, with
  any name for the player. The screen should say "Registered" for ten
  seconds and go back to the artwork; `curl
  http://p3a-fab.local/api/v2/makapix` should say `"registered": true`.
  Say when it is done (or do it together with a session, which can start
  it and watch).
- **M4.4-a, the pin icons.** With a Giphy, Klipy or museum artwork
  playing, swipe up: a pin icon should flash in the top left corner for
  four seconds (with Giphy art, the click icon top right as well). Swipe
  down: the unpin icon, top left. Swipe down again on the same artwork:
  the error icon, top right (there is nothing left to undo). On a file
  of your own, either swipe gives the error icon top right. The tests
  check what each swipe does to the list, not which icon is drawn.
- **M4.2-a, the credit under a Giphy artwork.** With a Giphy key set and a
  Giphy channel playing, wait for a GIF that is wider than it is tall.
  "Powered by GIPHY" should stand centred in the black band under it, in
  grey: readable from where the device is usually seen, quieter than the
  picture, clear of its edge. Nothing under a GIF that fills the screen.
  Say if the size or the grey is wrong.

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
- **M3.6-a, the card on a computer.** Plug a cable from the board's USB
  port marked for the P4's own USB (not the one the lab's serial console
  uses) into a computer. The device's screen should say the card is in use
  by the computer, and the computer should show a drive (labelled P3A
  after a format on the device). Copy a picture into its `p3a/animations`
  folder, eject the drive, unplug: the device should say it is restarting,
  restart, and play the picture in rotation. Worth one try of pulling the
  cable without ejecting.
- **M3.5-a, the overlays by eye.** Turn the clock on
  (`python host/p3a-lab/lab.py cmd "settings set {\"clock_enabled\":true,\"timezone\":\"EST5EDT,M3.2.0,M11.1.0\"}"`)
  and judge its size, place and outline over a few artworks; swipe up on
  an artwork (the error icon should flash top right for four seconds);
  tap to skip and watch for the small blue triangle bottom right while
  the next artwork loads.
- **M3.3-a, the screens by eye.** Restart the unit and watch the boot
  animation hand over to the artwork. Long press for the information
  screen. Then the format flow without formatting: run
  `python host/p3a-lab/lab.py cmd "storage latch"` four times, long press,
  tap "Format SD card", watch the button count down, tap "Cancel", and
  restart the unit (`lab.py cmd reboot`) to clear the latch. The "No
  usable SD card" screen needs the card taken out: power off, remove it,
  power on, tap "Format this card" (it should say no card was found), then
  put the card back.
- **M3.4-a, real fingers.** With something playing: tap the right half
  (next), the left half (back); hold a finger still for a little over two
  seconds (the information screen; the same again closes it); turn two
  fingers a quarter turn (the screen turns; turn it back); start a swipe
  and stop short (nothing should happen). Then press BOOT to pause and
  check that taps do nothing and a long press resumes.
- **M3.2-a, the look of the screens.** The font is chosen (Everyday
  Ample). Look at a screen on the device itself:
  `python host/p3a-lab/lab.py cmd "ui screen {\"screen\":8,\"up\":true,\"title\":\"Updating Wi-Fi\",\"text\":\"2.9.1 to 2.9.3. Do not unplug\",\"progress\":64}"`
  (take it down with `\"up\":false`). Sizes, colours and the banner's
  height are the numbers in `ui_style_default()`.
- **M2.4-a, the BOOT button pauses.** With something playing, press the
  BOOT button on the board once: the panel should go fully dark (backlight
  off). Press again: the same artwork is back at the brightness it had.
  `python host/p3a-lab/lab.py cmd "button"` shows the presses counted.

Cleared:

- **M2.1-a, how playback looks.** Cleared by Fab on 2026-11-23 (ledger entry of
  that date): motion, sharpness, JPEG tones, placement, transparency and
  rotation all look right.

## Open questions for Fab

Decisions that are still open, with the milestone that needs them. Details
and proposals are in [DECISIONS.md](DECISIONS.md) under "Open decisions".

| Id | Question | Needed before |
|---|---|---|
| Q1 to Q10 | Ten 1.x behaviours that look accidental (brightness not restored at boot, API keys readable over the LAN, no update check for 12 hours, and others). Keep or change each? Listed in [PARITY.md](PARITY.md) under "Behaviours to decide" | M1.3 at the earliest; each row names its milestone |
| O18 | Should the user's own files be scaled smoothly or with sharp pixels? 1.x used sharp pixels for everything on the card, which suits pixel art and makes photographs blocky. Options: keep sharp; go smooth; or decide per file (smooth for JPEG, sharp for the rest) | M2.4 |
| O2 | Should 1.2.5 carry a notice about 2.0, and with what wording? | 1.2.5 |
| O5 | Clock overlay: built with working answers (24 hour by default with a 12 hour setting, no seconds, hidden under any screen, one font). Say if any should differ | when convenient |
| O19 | How does a person tell the device its time zone? The setting is a POSIX TZ string, which nobody types. The web page could offer a list of regions, or send the browser's own zone | M5.1 |
| O6 | May p3a ship Giphy's and Klipy's own marks, and which? The source is credited in words meanwhile (D72) | when Fab has an answer; holds nothing |
| O20 | What should 2.0 tell the museums it is? 1.x sent "p3a/{version}" with a contact e-mail address. 2.0 sends "p3a/{version}" alone for now. Museums like a way to reach whoever runs a client; the choices are an address, the project's public page, or nothing | M4.3 ends; holds nothing |
| O21 | Large artworks are skipped when memory is short at a swap (measured: one swap in forty-five with 63 channels). Leave it, lower the 16 MB file limit, or let the outgoing artwork go first when memory is short? Details in DECISIONS.md | Before release; holds nothing |
| O1 | Exact name of the 2.x firmware asset: working answer D85, to confirm | M5.2 |
| O12 | OTA hardening choices: working answer D86, to confirm | M5.2 |
| O14 | A second board for the field test | M6.3 |
| O13 | How `v2` becomes `main` | M6.4 |

O9 was taken as a working decision (D73) on 2026-11-25.

## Ports from main

Fixes made on `main` after `v2` branched (at `4c9de368`) that 2.0 must pick
up by hand. Most 1.x fixes will not apply because the code is being
rebuilt; list them anyway and mark each "ported", "not applicable" with the
reason, or "pending".

| Commit on main | What | Status in 2.0 |
|---|---|---|
| (none yet) | | |

Expected: the lock-order fix for finding 25 (D9). Confirmed at M4.1
(2026-11-23): not applicable. 2.0 has no lifecycle lock. The scheduler has
one lock, and the content task never holds it across the card or the
network.

## Ports to main

Fixes found by the 2.0 work that Fab decided 1.x should have too. Each is
its own commit on `main`; nothing else of 2.0 goes there.

| Commit on main | What | From | How far it was tested on 1.x |
|---|---|---|---|
| `9466c035` | Hardware JPEG colour range; animated WebP soft edges (D47) | `7d1a00f8` | Built, and checked by eye on the dev unit on 2026-11-23 (ledger entry of that date). Nothing further is needed before 1.2.5 |

## Deletion log

What was deleted from `legacy/` without being moved, and why. Filled from
M0.1 on.

M6.2, 2026-11-25. Everything below was rewritten in 2.0 in the milestone
named, and was left in the quarry until its features had all moved. The
parity audit of this date is the check that they have (`PARITY.md`). 1.x's
sources remain on `main`.

- `components/play_scheduler/` (all of it): rewritten as 2.0's
  `play_scheduler` (M2.3, M2.4, M4.1): pure core, pick, turn and history,
  the content task. Its SWRR, LAi and refresh code were read, not moved.
- `components/channel_manager/`, except the `makapix_channel_*` files:
  the channel cache with its uthash tables, merge, eviction and
  verification sweep became `p3a_library`'s index, blob store and
  eviction (M2.2, M4.1). `psram_alloc.h` had moved earlier.
- `components/config_store/` (all of it): the settings blob and its
  per-source files became `p3a_settings`, generated from one schema
  (M1.4). Every key's 1.x name is in the schema's `was` field.
- `components/http_api/` (all of it): 7,400 lines of handlers across
  three naming eras became `p3a_net`'s route tables under `/api/v2/`
  (M1.5 to M5.2), each handler written with its feature.
  `http_api_rest_debug_sd.c` is not carried over (D94).
- `components/p3a_core/`, except `p3a_current_post` and
  `p3a_reaction_dispatcher` (kept until M4.5 part 4): the state machine,
  the render state and the touch router became `p3a_ui`'s model, screens
  and `ui_route()` (M3.1 to M3.4). `fresh_boot.c` (build-time switches
  to erase NVS and Wi-Fi) is not carried over: the lab's erase, the
  factory reset and a release's `nvs-erased.bin` do it.
- `components/ugfx/` and `main/ugfx_ui.c`: the uGFX graphics library
  (264 files) and the screens drawn with it. 2.0 draws its own text and
  screens (`ui_font`, `ui_canvas`, M3.2, M3.3).
- `components/mem_stats/`, `components/debug_http_log/`: 2.0 has
  `p3a_mem` (levels, the one-shot failure report) and `http_fetch`'s own
  logging of hosts.
- `main/` (what was left): `p3a_main.c` and `playback_service.c` (2.0's
  `main/p3a_main.c` is the boot sequence only), `animation_player.c`
  (rewritten at M2.1), `app_lcd_p4.c` (`p3a_board`), `app_touch.c`
  (`ui_gesture.c`, `ui_touch.c`), `sd_format.c` (`p3a_storage_format()`
  and `ui_card.c`), the 1.x Kconfig and manifest.
- `CMakeLists.txt` (1.x's root project with its release packaging and
  flasher build: `tools/make_release.py` and `host/p3a-flasher` now),
  `partitions.csv` (1.x's table), `dependencies.lock`.

Still in `legacy/`: `components/makapix`, `channel_manager`'s
`makapix_channel_*`, and `p3a_core`'s two files about the current post
and reactions, all for M4.5 part 4 to read; `sdkconfig` and
`sdkconfig.defaults`, which `tools/compare_sdkconfig.py` compares every
release build against.

M5.3, 2026-11-25:

- `flasher/build.ps1`: a manual PowerShell build of the flasher that
  `build_flasher.py` had already replaced in 1.x (its own README called
  it legacy). The rest of `flasher/` moved to `host/p3a-flasher`.

M5.1 and M5.2, 2026-11-25:

- `webui/` (what was left of it: `index.html`, `settings.html`,
  `playset-editor.html`, `ota.html`, `giphy.html`, `network.html`,
  `static/reactions/`): rewritten as `components/p3a_net/static/`. Each
  1.x page was one file with its script and style inline and its own
  copy of the shared helpers; the 2.0 pages are a page, a script, a
  model tested with node, and three shared scripts. `giphy.html` and
  `network.html` are tabs of the settings page. The reaction icons were
  copied in part 1 (re-encoded). `webui/museum/` was moved, not deleted
  (part 3).
- `components/ota_manager/` (all of it): rewritten as
  `components/p3a_update`. More than half of it was the web UI's own
  update (the `storage.bin` download, the LittleFS remount, the repair
  and recovery flows), which change A removes. The firmware half read
  three releases and compared three numbers; it looked for a `.sha256`
  file, marked a new image valid at the top of `app_main`, and left its
  screen up on two early failures (H8).

M4.4, 2026-11-25:

- `components/pin_lists/` (all of it): rewritten as `p3a_pins.c` and
  `p3a_pin_table.c` in `p3a_library`. Its three file formats (state,
  order, entry) and its copy of every artwork are one index file per
  list and a table; its garbage collector of orphaned copies has nothing
  to collect.
- `components/p3a_core/p3a_pin_dispatcher.c` and its header: what to pin
  is the record on the screen (`play_scheduler_current`), whatever its
  source; the per-source branches are gone.
- `components/show_url/` (all of it): rewritten as `show_url.c` and
  `show_url_name.c` in `play_scheduler`. Its task, its globals and its
  cancel flag are a job.

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

M2.3, 2026-11-23 (the legacy files stay until the rest of the scheduler is read out of them):

- From the playset: the `artwork` sub-struct of a channel, the legacy
  `pick_mode` and exposure bytes of the header, the default `name` per
  channel type, and the silent truncation of long fields.

M2.1 part 4, 2026-11-19:

- From the 1.x frame-trace endpoints: `POST /api/debug/provoke` and its
  four provocations (NVS, log, SD, CPU), and the two fields of the stats
  that named the SD busy-wait variant. In the history of `main` if a stall
  has to be provoked again.

M2.1 part 3, 2026-11-19:

- `legacy/components/loader_service` (its reader is `player_file.c`).
- From the 1.x loader and render files: the prefetch flags and semaphore,
  the directory scan helpers and index stubs that the channel system had
  already made dead, `animation_player_add_file` (returned "not
  supported"), the `PERF_DEBUG` blocks, the PICO-8 branch (PICO-8 returns,
  if at all, as a frame source of its own).
- Test hook `p3a_test_render.c`, replaced by `p3a_test_player.c`.

M2.1 part 2, 2026-11-18:

- From the display renderer: `display_renderer_deinit`, the display mutex,
  the render-mode enum and its enter and exit calls (now sources), the
  rotation setter that wrote the config store, the state variables of the
  processing, reaction and pin overlays (they return with those features),
  and the RGB565, `P3A_PERF_DEBUG` and single-worker code paths.

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

M3.3, 2026-11-23:

- `components/p3a_core/p3a_boot_logo.c` and its header: rewritten as
  `components/p3a_ui/ui_intro.c` on the settings store and the UI's model;
  the phases and timings are the same.

M3.5, 2026-11-23:

- `main/display_processing_notification.c`, `display_reaction_overlay.c`,
  `display_pin_overlay.c`: rewritten in `components/p3a_ui` (the indicator
  follows the scheduler's state; the icons are one overlay with two
  corners).
- `main/pin_icon_img.*`, `unpin_icon_img.*`, `reaction_submit_img.*`,
  `reaction_revoke_img.*`, `reaction_error_img.*`, `submit_click_img.*`:
  the pictures are now `components/p3a_ui/images/*.png` with their
  generated `.p3aimg` files.

M3.6, 2026-11-23:

- `main/app_usb.c` and `main/include/app_usb.h`: rewritten as
  `components/p3a_usb/p3a_usb.c` and `usb_session.c`. The PICO-8 stream
  and touch report parts are gone with PICO-8.

M4.1, 2026-11-23:

- `components/http_fetch/CMakeLists.txt`: the component is now
  `components/p3a_fetch` with its own.

M4.2, 2026-11-25:

- `components/giphy/` and `components/klipy/` (all files): rewritten as
  `components/source_giphy` and `components/source_klipy`. Their merge,
  cache-path, eviction-of-orphans, cooldown and key-latch code has one
  implementation now, in the content task and the blob store.
- `components/storage_eviction/` (all files): rewritten as
  `p3a_evict.c` and `p3a_blob_evict.c` (M4.1, D65). The part that dropped
  old channel files is `drop_stale_indices()` in `sched_content.c`.
- `components/channel_manager/download_manager.c` and
  `include/download_manager.h`: replaced by the content task (M4.1).

M4.3, 2026-11-25:

- `components/art_institution/museums/vam.c` and `smk.c`: rewritten as
  `components/source_museum/museum_vam.c` and `museum_smk.c`. Their page
  loops are the one in `source_museum.c`.
- `museums/ham.c` and `smithsonian.c`: rewritten as `museum_ham.c` and
  `museum_si.c`.
- `museums/rijksmuseum.c` and `art_institution_resolve.c`: rewritten as
  `museum_rijks.c` (its walk) and `walk_to_picture()` in
  `source_museum.c`.
- `museums/artic.c`: rewritten as `museum_artic.c`, without the second
  kind of query for deep listings (the museum cannot be played).
- `museums/common.c`, `art_institution.c`, `art_institution_refresh.c`,
  `art_institution_download.c`, `art_institution_rate_limit.c`,
  `art_institution_internal.h`, `include/*`, `CMakeLists.txt`: the
  dispatch table is `museum_util.c`; merging, eviction of orphans,
  downloads and cooldowns are the content task's (M4.1).
- `museums/wellcome.c`, `cma.c` and `mia.c`: rewritten the same way.
  Cleveland's table of cut department names is not needed (a term
  is 95 characters).

## Session log

Newest first. One entry per session or checkpoint: date, what was done,
what was verified and at which tier, the commit.

### 2026-11-26: M6.1 part 2, the memory soak with 63 channels

- **RUN-20261126-01**: the trace build, 63 channels across every source
  (the card, Makapix, 21 Giphy, 34 Klipy, 6 museum), 3.02 hours,
  109,834 frames. The jitter pass bar holds here too: no in-scope
  stall, no warning, no reset, no gap. p99 lateness 41.0 ms. Every
  channel listed and downloaded; the card did not livelock.
- **Eight artworks were refused for want of memory** (`cannot play ...
  ESP_ERR_NO_MEM`, about one swap in forty-five). The scheduler moved
  on each time, as it is written to (nothing is marked against the
  artwork). The first refusal's snapshot: 1.0 MB of external RAM free,
  the largest block 0.4 MB, for a file of 0.6 MB.
- **Where the memory goes, measured** (the status API now reports the
  external RAM: `memory.psram`). On the test build, player held: 25.7
  MB free with the card alone, 20.0 MB with the 63 channels (their
  indices take 5.7 MB, about 90 KB a channel), and steady over minutes.
  Playing, at a dwell of 5 s for nine minutes: free memory returns to
  18 to 20 MB between artworks, so nothing leaks; one large GIF takes
  up to 11 MB while it plays (the file is held in memory, with its
  frames); and at a swap two artworks are held at once, which took the
  minimum down to 1.1 MB. Two large files in a row are what was
  refused.
- **What this says about D91**: the budget (13 MB of indices) is the
  same worst case 1.x allowed (16 channels of 4,096), and it leaves
  about 13 MB for two artworks, less than this run had. A playset that
  fills it will skip large artworks more often than this one did. Not
  changed here: the remedy is not a smaller budget but not holding two
  whole files at a swap, or a lower limit on a file's size, and either
  is a decision. Recorded as O21.
- One SD read timeout in the three hours (`sdmmc_read_blocks failed
  (0x107)`, at 03:30): one download failed and was retried; the card
  stayed in service. One Klipy file of 22.5 MB was refused for size
  twice (over the 16 MB limit); a third refusal retires it.
- `soak.py` waits for a unit that has just been flashed to join the
  network (the first start of this run failed on that).
- Tested: the soak; `test_api.py` reads the new field (device tier, in
  the whole suite that followed).
- **The whole device suite after the audit's fixes: 359 passed, 2
  failed, 6 skipped** (33 minutes). The two:
  `test_setup.py::test_the_setup_network_is_on_the_air` again (the
  device reports its access point up; the workstation's scan does not
  list it: the third time on this LAN, never on the former one), and
  `test_giphy.py::test_a_swipe_up_on_a_gif_is_told_to_giphy`, where
  the swipe down did not take the pin out within ten seconds. The Giphy
  file then passed twice whole (8 of 8), so that one is intermittent
  and its cause is not known. Watch for it.

### 2026-11-26: M6.1 part 1, the jitter pass bar on 2.0

- **`host/jitter-lab/soak.py`** runs a soak against 2.0: it refuses any
  device but the dev unit and any build that does not trace, sets the
  workload (keys from the secrets file, never printed or saved), runs
  the UART logger and the frame puller for the hours given, analyses,
  and puts the unit back. 1.x's `soak.ps1` spoke 1.x's API.
- **The work mix is written down** (in `soak.py`): the card, Makapix
  promoted, Giphy trending and a search, Klipy GIFs and stickers, two
  museums. 1.x's runs used a playset of Fab's whose contents were not
  recorded; this is its stated intent, "the normal workload".
- **Result, RUN-20261125-02: PASS.** The trace build (release plus the
  frame trace, no test hooks), 3.03 hours, 98,090 frames, all eight
  channels listing and downloading: no in-scope stall of 100 ms or
  more, no warning (50 to 100 ms), no reset, no gap in coverage. p99
  lateness 47.5 ms, worst 158 ms.
- **What the late frames were**, read in the raw trace before accepting
  the verdict: 801 decode overruns of 50 to 158 ms from a few animations
  whose frames take about 100 ms to decode against a 30 ms frame time
  (out of scope by the pass bar's definition); and 60 stills drawn again
  mid-dwell, up to 23.9 s after they were first drawn. The analyzer had
  classed those as overruns too. A still is drawn once and nothing is
  presented while it stays unchanged, so that "lateness" is only the
  time since it was last drawn. `analyze.py` now names them ("stills
  drawn again") and leaves them out of the percentiles.
- `docs/jitter/REPORT.md` has the 2.0 row and what differs from 1.x's
  runs.
- **The device tests written during the soak have run**: smoke,
  scheduler, touch, player API, player and Makapix, 66 passed. That
  covers D91 (forty channels all loaded; 64 of the largest kind load 16
  and the other 48 say so), D93 (the long press ends the registration),
  the information screen's new row, the loop counts, the like call.
  One of them had to change: forty channels do not fit a line of the
  test console, so that test sets its playsets over HTTP.
- Not verified: free PSRAM with a large playset loaded (the memory soak
  is next and will show an index that could not be allocated); the
  recovery countdown and the setup screen's text (not testable here).
- Three PARITY rows ticked on these tests: 115 ticked, 39 open.

### 2026-11-25: M6.2 part 1, the parity audit and what it found

- **The audit.** A read-only agent went through the 89 unticked rows of
  `PARITY.md` and brought back, for each, the test or the ledger line
  that covers it, or what is missing. I checked a sample of the tests it
  named (14, all exist) and the claims I acted on, in the code. 46 rows
  were ticked on that evidence (seven of them dropped things confirmed
  absent, four re-marked as changed); 43 stay open, each now saying what
  it lacks. Most of the open ones wait on the registered Makapix player
  (M4.5-a) or on a manual check.
- **Found: a playset's 17th channel never played.** The design was
  "sixteen indices in memory, the rest on demand"; only the first half
  was built, and the channels left out said nothing. Now every channel
  loads while the indices fit a memory budget, and one that does not fit
  is reported on the home page (D91).
- **Found: the recovery restarts were silent.** When the Wi-Fi chip or
  its link dies the device counts down and restarts; the countdown went
  to the log only. It is on the screen now, and so is a notice when the
  device gives up and plays on without a network.
- **Found: a long press hid the registration screen and left the
  registration running** (D93).
- **Found: small things 1.x had.** The information screen says whether
  the device is registered and lists its recovery restarts if there were
  any; the setup screen gives both addresses and says when something has
  joined; a picture fetched from an address shows a progress bar on the
  screen; the recovery counters are cleared when an updated firmware is
  kept.
- **Found: the editor offered an offset for the SD card that the source
  ignored.** It no longer offers it (D92).
- **Tests added for what had none**: files that declare a loop count
  (GIF and WebP ignore it, APNG honours it), a like over HTTP.
- Verified: the test build compiles; host tests (43 programs); node
  (44 cases); the tree checks.
- **Not verified, because the dev unit is in a three-hour soak**: every
  device test written today for the above (`test_scheduler.py` many
  channels, `test_makapix.py` long press, `test_touch.py` rows,
  `test_player.py` loop counts, `test_player_api.py` like). To run when
  the soak ends, before anything else.
- Not testable here: the recovery countdowns and the give-up notice
  (the link cannot be made to fail on demand); the setup screen's text
  (the API is not reachable from the LAN in setup).
- Left open by the audit, not code: `followed_artists` and the server's
  playsets by name, `get_post` (M4.5 part 4); "reset the advance timer"
  as an action (1.x had it; nothing in 2.0 needs it: to ask Fab); Q8 (raw
  SD debug endpoints) and Q9 (`set_mirror`), still without an answer;
  the failure paths carried over on trust (SD self-repair, the stale
  index sweep, the Wi-Fi and link ladders).
- **Afterwards, `legacy/` was emptied of what 2.0 has rewritten**: 368
  of its 412 files, each group with its line in the deletion log. What
  stays is what M4.5 part 4 still reads, and the 1.x configuration the
  build check compares against. Q8 is answered by D94. `LICENSING.md`
  still lists uGFX, DejaVu and the PICO-8 player: to rewrite for what 2.0
  embeds.
- Correction to the entry above: the node tests are 42 cases, not 44.

### 2026-11-25: M5.4 part 2, the rest of the documentation

- **`docs/QUICK-START.md`**: the flasher's warning, registration and the
  key fields as the 2.0 pages word them, playsets at `/playsets`, the
  museum browser's steps as clicked earlier today.
- **`docs/HOW-TO-USE.md`**: addresses that moved, where downloads are
  kept (`blobs/`), the Giphy rendition settings (settings now, not
  Kconfig), the museums' waits (the device and the browser each keep
  their own), no PICO-8; the Firmware Updates section written new (one
  update, the trial, going back, pre-releases) and the REST API section
  written new for `/api/v2/`. Its examples were run against the dev unit:
  status, player, next, back, settings and its schema, playsets, pins,
  update, and an upload with `POST /api/v2/files`.
- **`docs/flash-p3a.md`** written new: a USB flash starts the device
  fresh; the three ways; the command as esptool 4 and 5 take it;
  building from source with the lab and `make_release.py`.
- **`tools/make_release.py` also writes `p3a-<version>-usb-flash.zip`**,
  the files a flash by hand needs in one download. The guide told people
  to download "the firmware zip", which no release had; the flasher
  program's "local ZIP" takes the same file.
- **`docs/infrastructure/`** is three pages now: an index, the
  architecture in one page (layers, the path from a source to the panel,
  pure logic and thin hardware, tasks and memory, storage, the network
  side), and the HTTP API (every route, from the route tables' own
  comments). The seven 1.x pages it replaces are deleted: they named
  components and files that no longer exist.
- **`docs/known-issues.md`** has a section at the top saying what became
  of each 1.x finding in 2.0, by milestone. It says of itself that it was
  compiled from the plan and the ledger and not audited again (M6.2).
- **`docs/deferred/`**: the aspect-ratio filter and Wellcome's long terms
  are marked done in 2.0 and kept as design records.
- No em dashes are left in the user's documentation.
- Not verified: the documents read by someone setting up a device with
  them (M6.3); the screenshot in the README is still 1.x's page.
- Left for the end of M5.4: nothing but that screenshot. `AGENTS.md` is
  current as of this commit.
- Afterwards: the screenshot was retaken (`images/screenshots/p3a-web-ui-2.0.png`,
  the home page with a promoted artwork playing). Taking it, the page
  looked cut off at phone width; measured in Chrome with frames 320, 360
  and 390 px wide, none of the four pages overflows (the settings tab bar
  scrolls sideways by design). The cut was headless Edge, whose window
  cannot be narrower than 500 px: screenshots narrower than that are
  crops, not layouts.

### 2026-11-25: M5.4 part 1, the README, and what a new device plays

- **Found while checking the quick-start guide against the code**: a 2.0
  device that was never told what to play played the files on its card
  and nothing else. 1.x shows Makapix Club's promoted artworks on first
  run, with no account, and the guide's fourth step is that. Now the
  default is two channels, the card's files and promoted (D90). It is
  not written down: a device told nothing stays one that was told
  nothing.
- **`README.md`** rewritten for 2.0 where 2.0 differs: the web interface
  as part of the firmware, the API under `/api/v2/` (the three examples
  were run against the dev unit), updates (one file, a way back,
  self-recovery), registration as the page words it, the layers and the
  storage layout in place of 1.x's component and partition tables, no
  PICO-8, a pointer to the migration page. No em dashes are left in it.
- Verified, device test (1 new case in `test_playsets.py`): with the
  active playset's file removed and after a restart, the channels are
  the card and Makapix promoted, promoted lists from the public feed,
  and nothing was written. Smoke, playsets and scheduler: 16 passed.
- Not verified: a truly new device's first minutes end to end (flash,
  setup, art on the screen), which is manual check M6.3's.
- The README still shows 1.x's screenshot of the web page
  (`images/screenshots/p3a-web-ui-2.20.png`): to retake.
- Left of M5.4: `docs/QUICK-START.md`, `docs/HOW-TO-USE.md`,
  `docs/flash-p3a.md`, `docs/infrastructure/`, `docs/known-issues.md`
  and `docs/deferred/` re-triaged, `AGENTS.md` final.

### 2026-11-25: M5.3 part 1, the release folder and the two flashers

- **`tools/make_release.py`**: reads a release build (`flasher_args.json`,
  the partition table, the app image) and writes `release/v<version>/`:
  `p3a-v2.bin` (D85), the bootloader, the partition table, the boot
  selection, the co-processor's image, `nvs-erased.bin` (D89),
  `flash_args`, `flash_command.txt`, `SHA256SUMS`, a README. No
  `storage.bin`, no manifest, no per-file `.sha256` (devices use GitHub's
  digest). 1.x did this inside the firmware's CMake, with the addresses
  typed in.
- It refuses what must not ship: a build with the test hooks or the frame
  trace, a `-dev` version (unless `--allow-dev`), a version that is not
  2.x, an image that is not whole or not p3a, an image over 90% of its
  slot, a layout that is not the 2.x partition table.
- **A USB flash erases the device** (D89). C6 says nothing carries over
  from 1.x, but the settings partition is at the same address in both
  layouts; `nvs-erased.bin` makes the erase part of the release, the same
  for every way of flashing.
- **The web flasher** (`docs/web-flasher/index.html`): a warning and a
  box to tick before a 2.x release can be flashed; a yes is for one
  release. It already read each release's `flash_args`, so the new layout
  and file names needed nothing else. It is served from `main`: the
  change goes live when `v2` becomes `main`. The workflow that copies a
  release's files to Pages (`publish-firmware-to-pages.yml`) takes its
  list from `flash_args` too and is unchanged.
- **The flasher program** moved from `legacy/flasher` to
  `host/p3a-flasher`. It writes what the release's `flash_args` lists
  (`flash_plan.py`) and holds no address and no file list (1.x had both,
  twice); it asks before a flash that erases; a release on GitHub is
  offered if it has a `flash_args`, which 1.x releases do. `build.ps1`,
  an older way of building it, is deleted (deletion log).
- Verified, self-tests (CI runs them): `test_make_release.py` (18 cases:
  what is packaged, each refusal, the asset name against
  `update_release.h`, the layout against `partitions.csv`),
  `test_flash_plan.py` (7 cases: parsing, refusals, overlap, a 1.x
  release's `flash_args`, and that what `make_release.py` writes is a
  plan the flasher reads).
- Verified by running: `make_release.py` on the real release build
  (refused as `-dev`; with `--allow-dev`, a folder of 9 files, the app
  at 21% of its slot) and on the test build (refused: test hooks).
- **Flashed onto the dev unit from the release folder**, with the
  command in its `flash_command.txt` and the IDF's own esptool. Before:
  the test build, on Wi-Fi. After: the release build, "settings: nothing
  stored; using defaults", "no Wi-Fi network stored", setup mode with
  its screen and the `p3a-setup` network, boot complete, no crash. The
  test build and the Wi-Fi were then put back.
- Found doing it: esptool 4 (what an IDF 5.5 install has) refuses the
  hyphenated spellings (`default-reset`, `write-flash`) that 1.x's
  `flash_command.txt` used and I had copied. The command is written
  with underscores now, which esptool 4 and 5 both take; the self-test
  holds it.
- Not verified: the web flasher in a browser (it needs the files on
  Pages or a local copy: manual check M5.3-a); the flasher program run
  or built (`build_flasher.py` needs a virtual environment with
  PyInstaller: M5.3-b).

### 2026-11-25: M5.2 part 2 and M5.1's end, the update page

- **The update page** (`update.html`, `static/update.js`,
  `static/update-model.js`): what runs, the newest release with its size,
  date and notes, check, install with the device's progress, the wait
  while it restarts and what became of what was asked (it runs, it was
  undone, it failed and why), the firmware on trial, going back to the
  other slot's firmware at any time, a version that was undone before,
  why an install cannot start. 1.x could not say most of that.
- The notes are GitHub's Markdown shown as text, never as HTML.
- Every page's Update tab shows a dot when a newer firmware is known
  (`ui.js`), and the home page a banner. The two `update_*` settings
  are on Settings > System with their words.
- `GET /api/v2/status` says what the board is: the chip and its
  revision, the co-processor's running firmware, what this firmware
  carries for it, and whether it is locked. The update page shows them.
  The dev unit's co-processor is on the known 2.7.0 lock (manual check
  M1.2-a), and the page says so.
- **`legacy/webui` and `legacy/components/ota_manager` are deleted**
  (deletion log). Known issues that went with them or were fixed on the
  way: H8 and M23 (part 1), H9 and M15 (the web UI update, change A),
  H11, M18 (`poller` keeps one timer), M19, M20 (the update keeps
  nothing in NVS), M29 (`ui.js`, `lib.js`).
- PARITY: the web and update rows are ticked, with what each still
  lacks.
- Verified, node (8 new cases, 43 in all): the page's reading of every
  state of the device, the wait, the outcome, times, notes.
- Verified, device tests: the page and what it loads are served; the
  status' new parts.
- **Clicked through in Chrome on the dev unit, against a mirror**: the
  page before a look; Check for Updates; the release offered with its
  notes and the dot on the tab; Install, with the bar following the
  device (15%, 86%); "The device is restarting"; then "Installed 2.0.1",
  the toast "Now running 2.0.1" and the way back offered; Go back, and
  the device on its first firmware again with the newer one kept in the
  other slot.
- Not verified: the page when an install fails or a firmware is undone
  (its model is node-tested, the device's side device-tested); the
  banner on the home page seen in a browser.
- **The whole device suite at the end of M5.1 and M5.2**: 350 passed, 6
  skipped, 2 failed, both in the tests. `test_setup` still expected "/"
  to be a 404 on the LAN, where it is the home page since M5.1 part 1;
  `test_card_flow` slept 1.5 s where it had to wait for a listing to
  end. Both corrected and passed alone. The release and trace builds
  pass.
- Seen afterwards and left: `test_the_setup_network_is_on_the_air`
  failed twice when run alone, after passing in the suite. The device
  reports its access point up; the workstation's own Wi-Fi scan did not
  list it within the test's 150 seconds. The code on that path did not
  change today. Likely the workstation's scan schedule on the new LAN;
  to look at again if it repeats in the next whole suite.

### 2026-11-25: M5.2 part 1, the firmware update

- **`components/p3a_update`** (net layer), written new; 1.x's
  `ota_manager` was read for what it did, not moved.
  - `update_release.c` (pure): GitHub's release list read into the one
    release this device could install. Only an asset named `p3a-v2.bin`
    counts (D85); a draft, a tag that is not a version, an asset still
    uploading, too small, larger than the slot, without a SHA-256
    digest, or at an address outside the repository's releases is passed
    over. Versions are ordered as semantic versioning orders them
    (`2.0.0-dev` < `2.0.0-rc.1` < `2.0.0`); 1.x compared three numbers
    and ignored the rest.
  - `update_policy.c` (pure): when to look (D87) and the trial's verdict
    (D86).
  - `p3a_update.c`: what runs and what the other slot holds (read at
    boot on `app_main`'s task), the look as a job, the clock, the trial,
    going back.
  - `update_install.c`: the image goes from the network into the other
    slot (D88). The first bytes must be a p3a image for this chip of the
    version the release named before the slot is opened; the whole
    download must have GitHub's SHA-256; the IDF verifies the slot. Every
    failure takes the screen down and leaves the device running (known
    issue H8).
  - `update_api.c`: `GET /api/v2/update`, `POST .../check`, `.../install`,
    `.../rollback`, each POST a job.
- **`p3a_internal_call()`** (`p3a_util`): one small long-lived task with
  an internal-RAM stack that runs the few calls that map flash
  (`esp_ota_begin`, `esp_ota_end`, choosing the boot slot, marking a
  firmware kept or refused). The install's job has its stack in PSRAM.
- **`http_fetch_to_stream()`** (`p3a_fetch`): a body handed to a function
  as it arrives, with the same retries, redirects, size rules and hash as
  a download to a file. The update no longer needs an HTTP client of its
  own. The manual-redirect address buffer grew to 2048: GitHub's signed
  asset addresses are about 900 characters.
- Two settings, group `update`: `update_auto_check`, `update_prereleases`.
- Test hook `!update source`: points the device at a mirror on the LAN.
  `p3a_update_set_source()` is reachable from nothing else.
- Verified, host tests (2 new programs, 13 cases, 43 programs in all,
  also under the sanitizers in WSL): the list GitHub gave on 2026-11-25
  (only 1.x: nothing for a 2.x device), the same list with 2.x releases
  beside it (`tests/assets/update/make_releases.py`), each reason a
  release is passed over, the order of versions on the specification's
  own examples, notes cut at a character, 1500 mutated lists; when the
  device looks; the trial's verdict.
- Verified, device tests (`test_update.py`, 8 cases, all passed): one
  look at GitHub's real list; a list that cannot be read; what is offered
  by version and by the pre-release setting; four bad downloads refused
  with their reason and the device carrying on (not an image, another
  version inside, wrong checksum, cut short); and the whole cycle from a
  mirror on the workstation: install with progress and the update
  screen, restart into the new firmware, on trial, kept once reachable,
  still kept after an ordinary restart, back to the first firmware. The
  "new" firmware is the test build with another version written into it
  and its checksum and hash put right.
- Found on the device: a firmware gone back to is itself on trial for a
  few seconds (the IDF marks any newly chosen slot so). It is kept the
  same way; the test waits for it.
- Not verified: a firmware that fails its trial (ten minutes without a
  network; its verdict is host-tested, the going back is the same call
  the roll back makes); an install from GitHub itself, with its redirect
  (manual check M5.2-a, needs a pre-release with a `p3a-v2.bin`); an
  install while the card is shared over USB being refused; the release
  and trace builds with the new component (at the milestone's end).

### 2026-11-25: M5.1 part 4a, museum titles on the home page

- An artwork's record says which museum it is from (`"museum": "vam"`,
  from `source_museum_id()`), and the home page's info panel asks that
  museum for the title, artist and date through the adapter the museum
  browser uses, loaded on demand. The source line names the museum.
- Verified, device test: `test_museum.py` asserts the field for each
  museum (run for V&A and SMK; smoke passed). Clicked in Chrome: a V&A
  drawing playing, the panel showing its artist and date from the museum.
- Clicked in Chrome as well (part 1's page, not clicked before): next,
  the info panel for a Makapix artwork, pin and unpin (the count
  following), pause and resume, each read back from the device.
- Not verified: the panel for the other eight museums.

### 2026-11-25: M5.1 part 3, the playset editor and the museum browser

- **The playsets page** (`playsets.html`, `static/playsets.js`,
  `static/playset-model.js`): the saved playsets (play, edit, delete) and
  the editor (name, balance bar, channels with weight and offset, reorder
  by buttons or dragging, save, save and play). A rename saves the new
  one and deletes the old. `/playsets#new` and `/playsets#edit=<name>`
  open the editor directly.
- The model holds what a playset must be, in the page's words, and the
  node test reads the limits out of `playset.h` and the source names out
  of `playset.c`: a limit changed in the firmware fails CI until the page
  agrees.
- **New against 1.x**: a pin list can be a channel (the active one or a
  named one); a name may have spaces and any letters (1.x allowed
  `[a-zA-Z0-9_]` only; the device refuses just slashes and control
  characters); Giphy and Klipy say in the dialog when their key is not
  set, with a link to Settings; every message is a toast (1.x's
  `showToast` was empty: known issue H11).
- **The museum browser** moved from `legacy/webui/museum/` to
  `static/museum/` (`git mv`, then changed), loaded only when it is
  opened. What changed:
  - `request.js` is the one place for what all nine adapters repeated: a
    museum's "slow down" (kept in the page; 1.x reported it to the device,
    which keeps its own account now), a bound on simultaneous requests,
    the two keys from `GET /api/v2/secrets/museum-keys`, and the length a
    term may have.
  - A term may be 95 bytes, a channel's identifier, where 1.x dropped
    every term over 32 characters. Cleveland's two cut department names
    are whole in `cma-terms.json` and the table that expanded them is gone.
  - The Rijksmuseum's 193 probes go out six at a time, and its walk stops
    at an empty page (known issue M19, both halves).
  - The list of museums is the device's (`GET /api/v2/sources/museums`):
    one the firmware lacks is not offered, one that cannot be played now
    (AIC) is shown with the device's reason.
  - A picked term comes back as `kind` ("vam:category"), `identifier`,
    `display_name`: a 2.0 channel.
- **Finding 19 (two museum tables kept by hand)** is closed without a
  `museums.json`: `tests/web/test_museum.mjs` imports the adapters and
  compares their ids and axes with the `.id` and `.axes` of each
  `museum_<id>.c`. A third file would have been a third copy.
- Verified, node (15 new cases, 35 in all): the playset model (limits
  from the header, identifiers by source, names, what stops a save, the
  balance bar, reordering, what is sent); the museums against the
  firmware; the terms files; cooldowns; bounded requests; what is offered.
- Verified, device tests (1 new case; smoke, web and playsets: 17 passed):
  the page, its scripts, every museum module and data file are served.
- **Clicked through in Chrome on the dev unit** (the extension is
  connected now): a new playset named in the page; the museum browser
  opened, V&A > Categories listed live with counts, Drawings previewed
  with its image and added; a Giphy search (the empty query refused with
  its toast, then added); the SD card; Klipy's 36 categories; a weight
  changed and the bar following (20/60/20); a channel moved; Save & Play.
  The device then held the playset exactly as built and was playing it
  (the museum channel listed 1024 artworks). On the settings page: a
  toggle saved and read back from the device both ways, a number out of
  range refused with the schema's message and not sent, the tabs and
  their hash.
- Not verified: dragging to reorder; the Makapix checks in the dialog
  (`verify-user` and the others, asked of the production site); Harvard
  and the Smithsonian in the browser (no keys on the dev unit); the
  other seven museums' listings after the port (their code changed only
  in the shared 429 line and the term length); the Rijksmuseum's bounded
  probes against the live service.
- Seen and left: with no Giphy key the channel shows `NO_KEY` on the
  device, as it should; the "Now Playing" badge wraps on a narrow list row.

### 2026-11-25: M5.1 part 2, the settings page

- **Drawn from the device's schema** (`settings.html`, `static/settings.js`,
  `static/settings-model.js`). The page holds no range, no default and no
  list of settings: each entry of `GET /api/v2/settings/schema` gets a
  control by its type (toggle, select, buttons, slider, number, color,
  key, time zone, text) and is checked against the same entry before it
  is sent. 1.x typed every limit twice and they had drifted (finding 6).
  What the page adds is what a schema should not hold: the words of
  help, names for some values ("4 hours" for 14400), and the layout.
- A setting the firmware gains shows by itself, in the tab of its group;
  one it loses is dropped. The node test reads the real `settings.json`:
  a key the page names that the firmware lacks, an option the firmware
  would refuse, or a default that fails the page's own check fails CI.
- **Keys** are shown as "Key set" or not, never as themselves (D33): the
  field is empty, typing replaces, "Remove" clears.
- **The time zone** is picked from a list of 34 regions, with "Other" for
  a POSIX string typed by hand. This is a working answer to O19 (a list
  on the page); the browser's own zone cannot be turned into a POSIX
  string without a zone database.
- **Makapix**: the registration code and its countdown are on the page
  too (1.x's page never showed them), with cancel, register again and
  remove; the link's state and the certificate's end.
- **System**: versions, uptime, the device's time, reboot (two presses),
  factory reset (new in 2.0; typed confirmation), and the crash record
  with its warning (D34) when there is one.
- The clock and playback settings, which 1.x did not have or kept under
  "Advanced", have sections of their own on the Display tab.
- Verified, node (9 new cases, 20 in all): every visible setting placed
  exactly once; controls by type; named options within the schema's
  range and including the default; what is typed; the zones' syntax.
- Verified, device test (1 new case): the page, its scripts and what it
  draws from are served; a value for every setting; a key reads back as
  the mask and is not in the answer's bytes.
- Looked at: three tabs in headless Edge at phone width.
- Not verified: any save made through the page in a browser (the calls
  it makes are the API tests'); the registration flow on the page; the
  other themes.
- Not done, and where it goes: the Wi-Fi co-processor's version and the
  board's facts on the System tab (the status call does not carry them
  yet: add with M5.2's update page); a list of nearby networks
  (`ideas.md` 4).

### 2026-11-25: M5.1 part 1, the home page

Started while M4.5 part 4 waits for the unit to be registered. The work
is on a different Wi-Fi network for some days (Fab's note); the lab's
secrets file holds it, and the former one under `wifi_2`.

- **Read first**: a digest of 1.x's web UI (320 KB of pages) and of what
  the review asks of it. D11 stands: the pages, the layout and the
  themes are kept; the internals change.
- **Shared scripts** under `components/p3a_net/static/static/`, as ES
  modules: `api.js` (one request function; an answer is the data or an
  error with its code; jobs; uploads with progress), `ui.js` (the bottom
  bar, toasts, a poller that is one timer and stops while the page is
  hidden, dialogs that close on Escape and on a click outside), `lib.js`
  (everything worked out from what the device says, with no DOM, so node
  can test it). This is finding 19's "one api.js" and audit M29; M18
  (pollers stacking) cannot happen with the one timer.
- **The home page** (`index.html`, `static/home.js`, `home.css` taken
  from 1.x's inline styles): the preview, previous and next, pause
  (now following the device), like, pin (now knowing whether the
  artwork is pinned), the info panel, now playing with a row per
  channel, shuffle, the pills (saved playsets, pin lists, built-ins),
  the pin dialogs, upload with a photograph shrunk in the browser,
  "show this address", and banners for what stands in a channel's way.
- **D83: the page gets an artwork's bytes from the device.** `GET
  /api/v2/blobs/{reference}.{ext}` (kept by the browser for good: the
  name is the content's hash) and `GET /api/v2/files/{name}`; a record's
  JSON names its address in `file`. 1.x had the browser fetch from each
  source, which put the Giphy and Klipy keys in the browser (D33 says
  they never leave the device) and showed nothing for a user's own file.
  Now every source previews, pins included, and opening the page
  contacts nobody but the device.
- `GET /api/v2/player` now carries what the page needs in one answer:
  each channel's kind, identifier, name and weight, whether shuffle is
  on, whether the artwork is pinned, and a number that changes when the
  pin lists do. `POST /api/v2/player/like` and `/unlike`.
- Pages may be asked for without ".html" (`/settings`), "/" is the home
  page (the setup page while the device is its own access point), and
  `/manifest.webmanifest` names the app after the device.
- Moved out of `legacy/webui`: `common.css`, `theme.js`, the favicon,
  the overlay icons and the app icons. The icons were re-encoded with a
  palette: 267 KB to 46 KB (the review: two thirds of the pages' weight).
- Verified, node (`node --test tests/web/test_lib.mjs`, 11 cases; added
  to CI): names, badges, problems, banners, which pill is active, what
  may be uploaded, shrinking, explanations.
- Verified, host tests: pages found without their ending (41 programs,
  also under gcc with ASan and UBSan).
- Verified, device tests (`test_web.py`, 4 cases): every file the home
  page loads and imports is served with its type, gzipped, and answers
  304; the manifest and its icons; a user's file served byte for byte
  under an escaped name, and paths that are not names refused; a blob
  served whose SHA-256 is its name; the player's answer; a pin made from
  the page shows in the answer. Also the API, player API, pins and
  network tests (two expectations updated for the changes above).
- Looked at: the page in headless Edge at phone width, playing nothing
  and playing a Makapix artwork (the Chrome extension was not connected,
  so nothing was clicked: the controls are not yet exercised in a
  browser).
- Not verified: any click in a real browser; the pages on a phone; the
  other four themes; an upload and a shrunk photograph through the page;
  the pin dialogs.
- Not done, and where it goes: titles for museum art in the info panel
  (with the museum scripts, part 3); a Giphy or Klipy title (the keys do
  not leave the device; a lookup by the device could give it); the
  "Followed" pill (needs a registered player: M4.5 part 4); the update
  banner and badge (M5.2); a channel's last listing time (the page shows
  when the next one is due instead).

### 2026-11-25: M4.5 part 3, the link, commands, views, likes, renewal

Everything a registered player does is written. What an unregistered unit
can exercise is tested on the device; the rest waits for M4.5-a.

- **The link** (`makapix_link.c`): MQTT over mutual TLS for commands,
  presence and views (D80). It is kept by the Makapix task's tick, once
  a second: connect when registered, the network is up and memory is not
  short; on a connection publish capabilities, then state, then status;
  status every 30 seconds; a view when one is due. The client's own
  task only copies what arrives into a queue and sets flags. A client
  is stopped and destroyed only by the task that made it (audit H13:
  1.x deleted its status task from the disconnect handler while it
  might be publishing; here there is no status task).
- **Reconnecting** is pure code (`makapix_link_logic.c`): 15 seconds
  doubling to two minutes, jittered; three failed handshakes, or one in
  which the player rejects the broker's certificate, call for one
  renewal; if that does not help the registration is "invalid" and the
  link stops until a renewal succeeds or the device registers again.
- **State is published whole** each time it changes. 1.x published one
  field at a time, each replacing the retained whole, so the server's
  retained state was whichever field changed last.
- **Commands** (`makapix_command_map.c` in `p3a_net`, pure; run by
  `net_makapix_commands.c`). The source hands each command, as it came,
  to a handler the net layer registers, on the Makapix task (audit M26:
  1.x ran them, a screen rotation included, on the client's task). The
  handler makes the calls the HTTP API makes: next, back, activate a
  playset, show, pause, a setting. The map knows the server's forms (its
  own source was the reference: `play_playset`, `show_artwork` with
  `native_format`), 1.x's (`execute_playset` with "name" to save too,
  `swap_to`, `show_url`, `set_background_color`), and a playset in
  p3a's own form. Unknown commands are ignored; `set_mirror` is answered
  "unsupported"; the three `set_*` are acknowledged.
- **"Show this post"** (`play_scheduler_show_artwork`): a job that
  shows an artwork a message names, fetching it into the blob store
  first unless a channel of the active playset already has it.
- **Views** hang off a new hook: the scheduler tells a source when one
  of its artworks is seen on the panel and when none is
  (`p3a_source_ops_t.shown`: not paused, no screen over it), with the
  channel it came from and whether a person asked for it. The count
  (first report after 5 seconds seen, then every 30) is pure code; time
  covered does not count.
- **Likes**: `react` for Makapix art is a job that asks through
  `makapix_rpc()`; the icon answers at once and the error icon follows
  if the server does not take it. No table of pending requests exists
  any more (audit H4 was a race in it).
- **Certificates** (`makapix_renew.c`): when one is due is pure code
  (from 190 days before the end, spread over a week; always with two
  weeks left; never without a clock). Looked at once a day, at every
  connection, and when the link asks. A renewal replaces the CA's
  certificate too.
- `GET /api/v2/makapix` adds the link's state, the post a view would be
  for, when the certificate ends and how its last renewal went.
- Verified, host tests (41 programs, also under gcc with ASan and
  UBSan): `test_makapix_link_logic` (8 cases: waits, the verdicts, view
  timing with covers, renewal dates, topics, every message read back as
  JSON, command envelopes cut and mutated), `test_makapix_command_map`
  (6 cases: every command in each form, and payloads that will not do).
- Verified, device tests (3 new cases; commands are handed to the
  source by a test hook, as the link would): the source is told which
  post is seen, and that none is when paused or when a file of the
  user's plays; next, back, brightness, rotation, background, pause, a
  channel, a playset, each doing what the HTTP call does, and bad ones
  doing nothing; a post the channel does not hold fetched from the vault
  and shown at its real size; `swap_to`; a post that does not exist; an
  address. Also the scheduler, content, pins, UI, overlay and player API
  tests, which share the changed code (42 passed).
- Not verified (needs a registered unit, M4.5-a): the connection itself
  and its TLS; what the server makes of status, capabilities, state,
  acknowledgements and views; a command arriving over MQTT; a like; a
  listing through `makapix_rpc()`; the token; a renewal. The handshake
  failure path cannot be provoked against production at all.
- Note for the tests: the test build holds the player still from boot
  with the same hold a covering screen uses, so under it nothing is
  "seen". `test_makapix.py` releases it.

### 2026-11-25: M4.5 part 2, registration

- **One long-lived Makapix task** (`makapix_register.c`), started at
  boot. It runs a registration when asked and will keep the link in
  part 3. 1.x made a task for asking for the code and another for
  polling, each on a static stack that a second start could hand to two
  tasks at once (noticed in the digest); requests here are flags the one
  task reads.
- **The flow**: ask for a code (`POST /api/player/provision`), show it,
  ask for the credentials every three seconds (404 until the owner has
  typed the code), save. Nothing reaches NVS before the credentials are
  whole, so a cancel or a code that runs out leaves the device as it
  was, registered or not. Starting again ends the one under way and
  asks for a new code. The answer that held the private key is wiped
  from memory after use, the parse tree's copy included.
- How long a code is good for is the server's word when the device's
  clock is set, else the quarter of an hour the server is known to give.
- **The screen**: the code is the title, drawn twice as large
  (`big_title`, new in `p3a_ui_content_t`, with a host test), over one
  sentence that says where to type it. The code is not logged.
- **Over HTTP**: `GET /api/v2/makapix` (registered; the phase, the code
  and the seconds left while one is shown; how the last one ended),
  `POST /api/v2/makapix/registration`, `DELETE` of the same (ends one
  under way, else forgets the registration). Not a job: it lasts until
  a person acts.
- A registration made or forgotten is an event
  (`P3A_EVENT_MAKAPIX_CHANGED`), on which the scheduler lists the
  playing Makapix channels again.
- Verified, host tests: the two answers read whole or not at all (ten
  kinds of bad code answer, five of bad credentials, room too small,
  PEMs too long); the large title. 39 programs, also under gcc with ASan
  and UBSan (which found a test that filled the content with 'W' bytes,
  the new flag included).
- Verified, device tests (1 new case, against production): a code asked
  for and shown (six letters and digits, 60 to 900 seconds, the screen's
  title); a second start gives another code; cancel returns to idle with
  nothing saved and the screen gone; forgetting a registration that is
  not there is refused. Looked at on a screenshot.
- Not verified: the owner's part and everything after it (credentials
  arriving, the save, "Registered"): M4.5-a. A code running out (15
  minutes). A registration over an existing one.
- Notes: each run of the test asks production for two codes (ten an
  hour are allowed from one address; the test skips when refused) and
  leaves two players that were never registered in the server's table.
  Whether the server clears those was not looked into.
- Not done, and where it goes: the notice for a Makapix channel on a
  device with no registration says "A source needs its key", which is
  not what is missing. Wording per source: with the web page, M5.1.
  Telling the server on unregistering (1.x did not either).

### 2026-11-25: M4.5 part 1, the Makapix source and the promoted channel

- **Read first: both sides of the contract.** The 1.x client (6,300
  lines) and the Makapix server's own source (it is on this workstation,
  beside this repository) were each digested before any code was
  written. The reference pages under `docs/reference/` disagree with
  both in many places; the facts used are stated in `makapix_parse.h`,
  `makapix_rpc.h` and `source_makapix.h`.
- **D80: a registered player asks over HTTPS, not MQTT.** The server
  answers the same request objects at `POST /api/player/rpc` with the
  player's bearer token (checked in production: 401 without one). 1.x
  published each question over MQTT and matched answers to waiters
  through a table, with a per-channel task and a poll loop around it
  (finding 8), a use-after-free in the table (audit H4), and a 128 KB
  reassembly buffer whose overflow the server "fixes" by dropping posts.
  In 2.0 a question and its answer are one `http_fetch` on the content
  task, like every other source. MQTT is left with what only it can do:
  commands, presence and views (part 3). O15 is answered by this: no
  server change is needed.
- `components/source_makapix`: `makapix_parse.c` (pure: a page of either
  kind into records, the questions, times, storage keys, where a file
  is), `makapix_rpc.c` (the exchange and the token: asked for when
  missing, replaced once when refused), `makapix_store.c` (the
  registration in NVS, nothing of it logged), `source_makapix.c` (the
  table).
- A channel is listed in the server's order (newest listing first;
  newest promotion first on "promoted": the feed-bump keys), 50 posts a
  page, paced under the server's 60 questions a minute, and stops when
  the playset changes.
- **Without a registration the promoted channel plays**, from the public
  feed, as in 1.x. The feed is asked for each post's address, so a GIF
  or PNG post is fetched as what it is: 1.x assumed WebP for every post
  on this path.
- A file's address is worked out from its storage key (two directory
  levels from SHA-256 of the key), as 1.x did. The Python reading of the
  recorded pages checks this against the address the server itself
  gives, for every post recorded.
- `GET /api/v2/makapix` says whether the device is registered. It does
  not give the player's key, which 1.x's status did: with the key anyone
  can ask Makapix for the player's token.
- Three tests used "makapix" as a source the firmware lacks. It lacks
  none now; they use a Makapix kind that does not exist, and expect the
  channel to fail and be passed over.
- Verified, host tests (`test_makapix_parse`, 7 cases; 39 programs, also
  under gcc with ASan and UBSan): two pages recorded from the production
  feed, and an answer to a player's question written by hand from the
  server's schema (a registered player is needed to record one: replace
  it then), against the Python reading; refusals told apart; times and
  storage keys read strictly; the questions; cut and mutated answers.
- Verified, device tests (`test_makapix.py`, 3 cases, against
  production): the promoted channel listed (32 records), every file
  fetched from the vault with no failure, pixel art on the panel; the
  other channels say NO_KEY; kinds that do not exist are refused without
  a request. Also the scheduler, player API and playset tests, which
  changed.
- Not verified: anything a registered player does (`makapix_rpc.c`, the
  token's rotation, the other five channels); a listing of more than one
  page on the device; a playlist post in a real answer.
- Not done, and where it goes: channels other than "promoted" for a
  device with no registration. The server lists them to anyone over
  HTTPS (`/api/v1/post/recent`, `/hashtags/{tag}/posts`, ...), which 1.x
  did not use. Recorded in `ideas.md`.

### 2026-11-25: M4.4, pin lists and "show this". M4.4 done

- **A pin is a record in a list, and nothing is copied** (D77;
  `p3a_pins.h`). A list is an index file like a channel's, flagged as
  the user's data, under `{root}/pins/`; a table beside them holds each
  list's slug and name and which one is active. 1.x copied every pinned
  artwork into the list's folder: a sixth store. What a pin does to the
  bytes now is keep them: the set of blob references that any list
  names is held in memory (sorted, counted per pinning), and eviction
  asks it for every blob, whoever called and whatever they passed, and
  again just before deleting.
- The table of lists and the kept set are pure (`p3a_pin_table.c`),
  with host tests: slugs never shared, names checked, the active list
  never deleted, 64 lists, a table from a file checked whole (eight
  kinds of damage each refuse it), the kept set through a thousand adds
  and removes.
- **A list plays as a channel** (`components/source_pinned`): source
  `pinned`, identifier the slug, empty for the active list. Each record
  keeps its own source, so it is drawn, credited and reacted to as that
  source's artworks are (D78, which answers Q4: pinned museum and Giphy
  art is scaled smoothly, as in its own channel). The pin store emits
  `P3A_EVENT_PINS_CHANGED` and the scheduler lists a playing list again.
- **Over HTTP** (`net_pins_api.c`): the lists, a list's pins by page,
  make, rename, make active, delete, pin and unpin what is on the
  screen, unpin an entry. An entry is named by its position and its key
  together and an unpin whose two do not match is refused: 1.x filtered
  by a number that could collide (audit M16).
- **A swipe up pins, a swipe down unpins** (`ui_touch.c`), beside what
  the artwork's source makes of the swipe: the source's icon top right
  at once, the pin's top left when the card has been written (a job). A
  swipe nothing could act on gets the error icon.
- **"Show this"** (D79; `show_url.h`): `POST /api/v2/player/show` with
  an address answers 202 with a job that fetches into the temporary
  folder, looks at the bytes, and moves the picture among the user's own
  files under a name made from the address, then plays it. What is not
  a picture never reaches the user's folder. The bytes decide the
  extension, so an address with no extension works (1.x refused it).
  Each request is its own job with its own copy of the address (audit
  M11). The naming is pure with a host test.
- **Fixed, found on the way: route tables were dropped without a word.**
  The HTTP server held eight tables and the pins table was the ninth:
  the test hooks' routes, registered last, were simply not served. Room
  for sixteen, and a refused table is now an error in the log.
- Audit H18 (a header describing an older format) goes with the header.
- Verified, host tests: 2 new programs (`test_pin_table`,
  `test_show_url`), also under gcc with ASan and UBSan.
- Verified, device tests (`test_pins.py`, 8 cases; one more in
  `test_giphy.py`): there is always a list; lists made, renamed,
  deleted, the active one never; the artwork on the screen pinned once,
  a file of one's own refused; swipes down and up; a list playing as a
  channel and following an unpin; a list that does not exist is a failed
  channel; eviction asked for everything leaves the pinned blob; the
  lists and what they keep after a restart; unpinned, the blob goes like
  any other; a picture at an address fetched, kept and played, a web
  page and a missing file refused, bad addresses and a paused player
  refused. Suite: the whole suite: 330 passed, 6 skipped; the pin tests again on the last build.
- Not verified: which icon a swipe draws (M4.4-a); a full list (4096)
  and 64 lists on the device (host-tested limits); pinning with the card
  handed to a computer; a Makapix artwork pinned (M4.5); "show this"
  with a large file, a redirect, or the device short of memory.
- Seen once, not explained: Giphy's analytics server answered 400 to the
  click in `test_giphy.py` in one run and 200 in the next.
- Not done, and where it goes:
  - A pinned artwork whose bytes turn out damaged is deleted like any
    damaged blob and its pin then cannot play; nothing fetches it again
    unless its own channel does (the same bytes get the same reference,
    and the pin plays again). Fetching for a pin list itself: with the
    web page's pin dialogs, M5.1.
  - The browser fetching a pinned artwork's file for a preview, and the
    pin dialogs: M5.1.
  - Progress of a "show this" fetch on the device's screen (1.x drew it
    when asked to): the job reports its percentage; drawing it is
    M5.1's, with the page that asks.
  - Makapix's "show URL" command: M4.5, through the same call.

### 2026-11-25: M4.3 part 4, the Rijksmuseum and AIC; O10. M4.3 done

- **The Rijksmuseum** (`museum_rijks.c`). Its listing says only which
  artworks a set holds; where a picture is takes a walk of three Linked
  Art documents. A museum may now have a `walk` in place of `image_url`:
  a pure function that, step by step, is given the document it last
  asked for and names the next address, until it names the picture.
  `source_museum.c` fetches the documents. Each address is checked to be
  one of the museum's own before it is fetched, and the picture's to be
  at its image server.
- **A Rijksmuseum record keeps the artwork's number as its key** (D75).
  1.x wrote the picture's id over it once the walk had been done, with
  markers for "not walked" and "failed". In 2.0 a record's identity must
  not change (a fresh listing finds what the device has by it), so the
  walk is done when the picture is fetched, which is once.
  `P3A_RECORD_UNRESOLVED` has no user.
- Its listing pages by token: the address each page gives for the next
  (`museum_page_t.next`), asked only if it is the same search. A
  channel's offset is not honoured for it.
- **AIC** (`museum_artic.c`) is in the table with `unavailable` set: its
  channels report that they cannot be played and nothing is asked of it.
  A device test asks its image server for a picture and expects the
  refusal (403); the day that test fails, the museum can come back.
  1.x's second kind of query, for listings past 1,000 artworks, is not
  carried over while it cannot be played.
- **`GET /api/v2/sources/museums`** lists the museums from the firmware's
  one table: id, name, axes, whether one needs a key and has it, and why
  one cannot be played. The web page (M5.1) reads this instead of keeping
  a list of its own.
- **O10, decided (D76): no time budget for JPEG decodes.** Measured on
  the device, from "next" to shown, ten pictures each: Cleveland (893 to
  1216 pixels) 1.0 s median, 1.3 s at worst; Minneapolis (800) 0.9 s;
  V&A and the Rijksmuseum (720) 0.8 s. The sizes p3a asks the museums
  for bound the time, and a slow decode holds up only the next artwork,
  never the one on the panel.
- **Fixed, found by the device suite: a listing can be stopped.** A
  museum channel of a thousand artworks takes eleven requests to list
  (25 seconds); a playset activated meanwhile waited behind it, because
  the content task does one thing at a time and a listing could not be
  interrupted. A source is now given a way to ask whether its listing is
  still wanted (`stop` in `p3a_channel_ref_t`), asks between requests
  and hands it to its requests as their abort check. A device test
  changes the playset three seconds into a long Cleveland listing and
  sees the new one playing within eight.
- The suite's test that saved a museum playset as the active one (from
  when no museum source existed) now names a source the firmware lacks.
- The rest of `legacy/components/art_institution` is deleted (deletion
  log).
- Verified, host tests (36 programs, also under gcc with ASan and
  UBSan): the Rijksmuseum's recorded pages and its three recorded
  documents walked to the picture's address, with documents that name
  somewhere else, the wrong step, and 600 mutated copies of each; AIC's
  recorded page; every axis a museum is said to have.
- Verified, device tests (5 new cases): a Rijksmuseum set is listed, its
  pictures found, fetched and played; AIC is refused and its image server
  still refuses the device; the list of museums over the API; a playset
  change during a long listing. Suite:
  the whole suite on the last build: 322 passed, 6 skipped.
- Not verified: a Rijksmuseum set of more than one page on the device
  (the tests keep 32 artworks); an artwork whose documents name more than
  one picture (the walk takes the first); any museum's rate limit; the
  museums' channels across a restart (the content task's index file is
  tested with the test source).
- Open: `test_overlays.py`'s check of the red failure mark failed
  once more in a whole-suite run (the third time in about thirty), with
  the corner black. Not explained; the test's message now carries what
  the device says is on its screen.
- Not done, and where it goes: the browser telling the device that a
  museum rate-limited it, and the device's cooldowns shown to the browser
  (1.x had both): with the web page, M5.1. The device's own waits are the
  content task's (D66).

### 2026-11-25: M4.3 part 3, Harvard and the Smithsonian

- `museum_ham.c` and `museum_si.c`: the two museums that need the user's
  own key (`harvard_api_key`, `smithsonian_api_key`). The key is for
  listing only; pictures are fetched without it.
- Without a key the channel reports `NO_KEY` and the museum is not asked.
  A key the museum does not know is `AUTH` (Harvard answers 401, the
  Smithsonian 403). Saving the right key lists the channel by itself.
- Harvard's picture address sends the request on to another server: the
  first use of `redirects` on the device.
- A key may hold a colon: Harvard's are names like `urn-3:HUAM:79762_dynmc`.
- The Smithsonian's unit code is checked to be letters and digits before
  it goes into the query.
- The recorded listings were made with the test rig's keys; Harvard
  repeats the key in its "next page" address, which is cut out of the
  recording, and a check finds no part of any key in the recorded files.
- Verified, host tests (36 programs, also under gcc with ASan and
  UBSan): both museums' recorded listings against the Python reading,
  addresses, a refusal and the Smithsonian firewall's page read as "not a
  listing", mutated listings.
- Verified, device tests against the museums (2 new cases): for each,
  no key, a wrong key, the right key, then listed, fetched and played;
  neither key is in the device's log.
- Not verified: the Smithsonian's units other than SAAM; a rate limit
  from either; the whole device suite on this build.

### 2026-11-25: M4.3 part 2, Wellcome, Cleveland, Minneapolis

- Three more museums, each a pure file of about 120 lines and a row in
  the table: `museum_wellcome.c`, `museum_cma.c`, `museum_mia.c`.
- **Cleveland's and Wellcome's 1.x workarounds are gone**, as planned: a
  term is 95 characters in 2.0, so Cleveland's long department names need
  no table to expand them and Wellcome's long labels are not hidden.
- Cleveland and Minneapolis have no IIIF: Cleveland's one "web" picture
  (750 to 1300 pixels) and Minneapolis's 800-pixel bucket are fetched.
- Minneapolis answers a bare `[]` past the end of its index: read as an
  empty last page. A term with a quotation mark or a backslash is refused
  (it would end the phrase in the query).
- **A refused download is the artwork's, for a source that says so**
  (`refusal_is_the_artworks`): about one in twenty of Minneapolis's
  listed works answer 403 for their picture. That counts against the one
  artwork (left alone after three) and the museums' other downloads go
  on; for Giphy and Klipy a 403 still stops the source for 15 minutes.
- Verified, host tests (36 programs, also under gcc with ASan and
  UBSan): the three museums' recorded listings against the Python
  reading, their addresses, mutated listings.
- Verified, device tests against the museums: all five list a channel,
  fetch pictures and play them (3 new cases, 12 with the smoke tests).
- Not verified: a refused Minneapolis picture on the device (none came
  up in the 32 listed); Wellcome's label axes on the device (host-tested
  addresses only); the whole device suite on this build.

### 2026-11-25: M4.3 part 1, the museum source with V&A and SMK; D on the device

- `components/source_museum` (layer `sources`), registered in `app_main`.
  **One loop lists a channel for every museum** (`source_museum.c`): the
  pages, the key, the offset, the pause between pages. 1.x had that loop
  nine times, about 250 lines each.
- **A museum is a row in a table with three pure functions** in its own
  file (`museum.h`): the address of a page of a listing, that page read
  into records, and where a record's picture is. V&A (`museum_vam.c`) and
  SMK (`museum_smk.c`) are in; each is about 100 lines where its 1.x file
  was 400.
- A channel is kind `{museum}:{axis}` with the term as its identifier, as
  in 1.x. A museum or an axis this firmware lacks is the channel's error.
- A record's key goes into the picture's address as it is, so only keys
  made of letters, digits and `- . _ ~` become records. When the listing
  gives the original's size (SMK), the record gets the size of the
  720-pixel picture that will be fetched: the aspect filter then passes
  an elongated artwork over before it is downloaded.
- Downloads follow redirects by hand for a source that says so
  (`redirects` in the source table), as 1.x did for the museums.
- **The museums are told "p3a/{version}" and nothing else.** 1.x sent a
  contact address with it; what 2.0 sends is O20.
- **Item D, seen on the device.** The test source serves a 5:1 strip (cut
  from a V&A image by its IIIF server): with the filter on it is loaded,
  measured, logged as "too elongated: 720x144" and never shown in 8
  turns; with the filter off it plays.
- Verified, host tests (1 new program, 36 in all, also under gcc with
  ASan and UBSan): both museums' recorded listings against the Python
  reading; kinds and axes; every address, with a term that tries to add a
  parameter or close SMK's filter bracket; keys; sizes; what is not a
  listing; cut and mutated listings.
- Verified, device tests against the museums (4 new cases in
  `test_museum.py`, 1 in `test_content.py`): a V&A category and an SMK
  collection are listed (32 records), pictures are fetched and play as
  stills no larger than the panel, as JPEG; unknown museums and axes are
  refused; D as above. With the smoke and content tests: 20 pass.
- Not verified: a listing of more than one page on the device; an offset;
  a museum's rate limit; a redirected download (neither museum's image
  server redirected); the whole device suite on this build (it runs at
  the end of the milestone).
- Found on the way: SMK's server leaves an intermediate certificate out
  of its chain. Python on the workstation refuses it; the device's
  certificate bundle accepts it (the listing and the downloads work), as
  does curl.

### 2026-11-25: M4.2 part 2, internal memory under a listing (O9). M4.2 done

- **Measured** with `host/p3a-lab/measure_content_memory.py`: a fresh
  boot, a Giphy trending channel of 100 GIFs in the larger rendition
  listed (two pages), downloaded and played with a new artwork every 5
  seconds, for 7 minutes. The internal pool is 391 KB and has 240 KB free
  when idle.
  - As built: sampled every 2 seconds the pool never showed less than
    203 KB free, but its lowest point was 60 KB: a dip of 170 KB between
    two samples, under the 64 KB mark at which optional work is refused.
  - With JSON parse trees in PSRAM: lowest point 201 KB. Same run
    otherwise: 72 artworks shown, none failed, no download failed.
- **JSON goes to PSRAM** (`cJSON_InitHooks` at the top of `app_main`,
  D73). cJSON makes one small allocation per value, and small allocations
  come from internal RAM; a listing page is 100 to 250 KB of JSON. 1.x did
  the same for the same reason and 2.0 had not carried it over.
- O9, decided on those numbers (D73): transfers are serial (D63), work is
  shed below the marks (`p3a_mem_level()`), JSON is out of the pool, and
  every task stack but the render tasks' is in PSRAM (the task helper).
  **Packet mode is not adopted**: with 200 KB of headroom the on-demand
  receive buffer has room, and packet mode costs receive throughput on
  every device to remove an assert that needs the pool nearly full. The
  1.x thresholds stay. The soak (M6.1) watches the pool's lowest point
  and reopens this if it falls under 100 KB.
- Verified: the two measurement runs above (device, by hand); the whole
  device suite on the build with the change: the whole suite: 309 passed, 6 skipped.
- Not verified: the pool under Klipy's larger pages (250 KB of JSON; the
  parse no longer touches the pool, so the Giphy figure should hold);
  with Makapix's MQTT session up (M4.5); over hours (M6.1).

### 2026-11-25: M4.1 closed; M4.2 part 1, Giphy and Klipy

- **M4.1's owed runs.** The whole device suite on the M4.1 commit
  (`4cb5d4b5`): 297 passed, 6 skipped. Its CI run is green. The release
  and trace builds pass the build checks; they were made after M4.2
  part 1 was in the tree, so they cover both.
- **Two sources**, `components/source_giphy` and `components/source_klipy`
  (layer `sources`), each a `p3a_source_ops_t` and one registration in
  `app_main`. Each is two files: the requests (`source_*.c`) and what the
  answers mean (`giphy_parse.c`, `klipy_parse.c`: pure, host-tested). A
  source lists its channel into an empty index and says where an
  artwork's bytes are; when to list, the downloads, the waits and the
  index on the card are the content task's (M4.1). Written anew from the
  1.x code rather than moved: the 1.x files were mostly merging, caching
  and cooldown bookkeeping, which now have one home.
- **Giphy**: kinds `trending` and `search` (the identifier is the query);
  the offset starts the listing further down. Rating, country, cache size,
  refresh interval, rendition, format and prefer-downsized are the
  settings that were already in `settings.json`.
- **Which file of a GIF is downloaded is in its record**
  (`identity.giphy.file`, D68), decided when the channel is listed. The
  download address then needs only the record, and a change of the
  rendition setting takes effect at the next listing. The
  downsized-medium rule is 1.x's: taken when the listing says the file
  exists (its own address is read, never guessed) and it is no larger
  than a screen and a half each way.
- **Klipy**: kinds `gif:` or `sticker:` with `trending`, `search` or
  `category`. Its key is part of the address's path, a key it does not
  know is a 404 (reported as `AUTH`), and an item's address is asked for
  at download time (`ops->url` makes a request), as in 1.x. Advertisements
  in a listing are skipped. Only an https address at `*.klipy.com` is
  downloaded: the address comes from a response.
- **Dates are read as UTC.** 1.x used `mktime()`, which applies the time
  zone the clock overlay sets.
- **The fetcher logs only a URL's scheme and host** (D69). Giphy's key is
  in the query and Klipy's in the path. Device-tested: neither test key is
  in the device's log after a run.
- **A swipe up on a GIF is told to Giphy** (its click analytics), through
  a new entry in the source table, `react` (D70): the UI asks the source
  of the artwork on the panel and shows the icon for what it answers. The
  two requests run as a job. `POST /api/v2/sources/giphy/reset-id`
  forgets the id Giphy gave the device.
- **A source's trouble is said over the artwork** (`ui_player.c`, pure,
  D71): a refused key stays up, by the source's name, until it is put
  right; a missing key and a rate limit are said once for 20 seconds, at
  most twice per boot.
- The player's state shows a record's `format` once known.
- **The source is named under its artwork** (idea 2, D72): when a Giphy
  or Klipy artwork leaves a band free below it, "Powered by GIPHY" or
  "Powered by KLIPY" is drawn there in the UI's font, grey, at three
  times its size or two, centred. Never over the picture: an artwork that
  fills the screen, or leaves less than 26 rows, has no credit. The line
  is the source's (`credit` in its table), where it goes is
  `ui_credit.c` (pure), and the setting `source_credit` turns it off. It
  is words, not the services' logos: see O6.
- **The information screen** has a row for each remote source of the
  active playset: OK, Waiting, No key, Key refused, Asked to wait, Not
  reached.
- **An offset past the end of a channel starts from the top.** Giphy's
  trending is under 200 GIFs; an offset beyond that used to give an empty
  channel.
- **Fixed, found by the device suite: a new playset whose first pick fails
  now starts when something arrives.** `start_wanted` was cleared when the
  first request was made; if that failed (the index on the card named
  blobs that had been evicted) the previous playset's artwork stayed up
  for a whole dwell. It is cleared when an artwork is shown.
- **A hang in the suite, explained: it was the test rig.** Twice the
  first request of a Klipy playset stayed "loading" for minutes. The
  scheduler now logs an error when a request has had no answer for 30
  seconds, with what both sides believe, and the second time that line
  said the artwork was loaded and staged and the display never took it.
  The `!lcd` test patterns hold the panel on the test source until
  something takes them down, and nothing did: every test that then
  waited for the scheduler's own artwork hung, unless a test in between
  happened to switch the source. `!lcd done` hands the panel back, and
  the two test files that draw patterns call it when they finish. A
  release build has no test source. The log line stays: a request that
  is never answered should not be silent.
- `tests/assets/sources/` holds responses recorded from both services
  with the requests the device makes. No key is in them, and Giphy's are
  cut down to the fields the device reads, with the analytics tokens of
  the account that recorded them taken out. Beside them is
  `make_expected.py`, a second reading of them in Python that the host
  tests compare the C parsers against.
- Verified, host tests (3 new programs, 35 in all): both parsers on the
  recorded listings against the Python reading; items that cannot be
  used; which file is taken; dates; every address p3a builds, with a key
  or a query that tries to add a parameter or a path; cut and mutated
  listings (3000 and 1500 rounds); the notices' words; where the credit
  goes (`test_ui_credit.c`).
- Verified, device tests against the live services with the test rig's
  keys (11 new cases, `test_giphy.py` and `test_klipy.py`): no key says
  so, on the channel and on the screen; a key Giphy refuses is `AUTH`,
  Klipy's 404 is `AUTH`; saving the right key lists the channel by itself;
  GIFs are downloaded and play animated at the size asked for; a search
  channel; a sticker search as WebP; an unknown kind is the channel's
  error; the swipe up is told to Giphy; the id is forgotten and asked for
  again; the refused-key notice is up over a playing file, with the
  "Key refused" row on the information screen, and goes when the key is
  right; the credit is in the band below a wide GIF and nowhere above
  it, and goes when its setting is turned off. In `test_content.py`, one
  more: a playset whose blobs all went starts as soon as the first is
  fetched again. Suite: without its slow tests, 290 passed on the build before the credit was added; on the last build the smoke, Giphy, Klipy, settings, overlay, UI and content tests pass.
- Not verified: the credit by eye (its size and grey were chosen without
  seeing the panel: manual check M4.2-a), under a Klipy artwork, and
  with the screen turned; a rate limit from either service (the waits are
  host-tested; none was provoked); listings of more than one page on the
  device (the tests keep 32 items); the downsized-medium choice on the
  device (the tests turn it off to keep downloads small; host-tested on
  the recorded listings, where every one of the 50 items takes it); Klipy
  categories; a GIF Giphy has removed; the offset's effect on what is
  listed; the once-only notices for a missing key and a rate limit over
  an artwork (host-tested words, model cap host-tested); the click icon
  by eye.
- Known and left: a channel loaded from its index believes in blobs that
  eviction has since deleted, and finds out one failed pick at a time
  (each is then fetched again). It mends itself, noisily. A Klipy download
  costs one request for the item's address, as in 1.x; the listing
  already had that address. Keeping the addresses of the last listing in
  memory would save it (an idea, not built). A source's `refresh()`
  cannot hand the server's Retry-After to the schedule; neither service
  sent one worth having (Giphy's 401 says 0), and the schedule's floor is
  15 minutes.
- From `docs/known-issues.md`, for the files that left `legacy/`: the
  inline SDIO wait in `giphy_download.c` is gone (downloads use the
  fetcher's `wait_on_sdio`); the comment drift in `giphy.h` went with the
  file.

### 2026-11-23: M4.1 parts 2 and 3, the content task and eviction

- **The content task** (`sched_content.c`, class `NETWORK`): the one task
  that fills the scheduler's channels. One pass does one thing, in this
  order: load a channel's index from the card, list a channel, save an
  index, make room on the card, download an artwork. One transfer at a
  time (D63). The listing job on the I/O runner is gone: local channels
  are listed here too.
- **Listing.** A source's `refresh()` now fills an empty index with what
  the channel holds; `p3a_index_carry_over()` (pure) brings over what the
  device knew about those artworks: the blob, the format, the size, the
  failure counts. One implementation, where 1.x had three.
- **When** (`sched_refresh.c`, pure): at the source's interval; after a
  rate limit, what the server asked for and at least 15 minutes; with no
  key or a refused key, an hour or until a setting changes; after any other
  failure one minute, doubling to an hour; never again once the card has
  failed. A remote channel is not listed without a network, and what
  failed for want of one is tried when it returns (D66).
- **Indices on the card**: `{root}/channels/{channel id}.idx`, saved at
  once after a listing and half a minute after a download changes one. A
  channel the device has seen before plays at boot from that file, before
  the network is up, and is not listed again until the file is as old as
  the source's interval (D64). Indices of channels no playset has played
  for 60 days are dropped once per boot.
- **Downloads**: round robin over the channels, the next record without a
  blob. The source says where (`ops->url`), `http_fetch` writes and hashes,
  the first bytes say what it is (a page served with 200 is refused), the
  blob store takes it. A 404 marks the record gone for good; a refusal
  (rate limit, key) stops that source's downloads for 15 minutes; anything
  else counts against the artwork (three and it is left until the restart)
  and pauses downloads for 10 seconds, doubling to 5 minutes. Nothing is
  downloaded without a network, while internal memory is short
  (`p3a_mem_level()`), onto a failed card, or with less than 16 MB free.
- **Eviction** (`p3a_evict.c` pure, `p3a_blob_evict.c`): when free space
  falls below the start mark the oldest blobs go until the stop mark is
  reached. The marks are 1.x's. One walk and a sort, where 1.x walked up
  to eight times with a halving age (D65). Spared: blobs younger than four
  hours, and what the caller keeps (the artwork on the panel and the one
  being loaded; pins join at M4.4). The free space is measured again every
  32 downloads.
- **A record whose blob is gone** (evicted, or removed by hand) is found
  when it is played: the reference is cleared and the artwork is fetched
  again. Found by the device suite: such a record was set aside and never
  fetched.
- **The scheduler shuts down cleanly under a download**: a transfer gives
  up at its next chunk when the playset changes or the card is handed to a
  computer, and `play_scheduler_shutdown()` waits for the task to be idle.
- `POST /api/v2/player/refresh` lists every channel of the active playset
  now. The player's state reports `downloading`, per channel `listings`,
  `refresh_in_s` and `to_download`, and the counters `downloads` and
  `download_failures`.
- **The screen** says "Downloading artworks", "Waiting for the network",
  "A source needs its key" or "A source asked to wait" when nothing can
  play for one of those reasons (`ui_player.c`, pure).
- A source for tests (`p3a_test_content.c`, channel source `test`, D67):
  the test sets what it lists and how a listing ends.
- The scheduler's wiring is two files now, with the state in
  `sched_state.h`.
- Verified, host tests (3 new programs, 32 in all): the refresh schedule
  (7 cases), eviction (5: the marks, oldest first, young and kept spared,
  wrong dates, file names), the carry-over, the new words on the screen.
- Verified, device tests (9 new cases in `test_content.py`, against real
  files on a public server): a remote channel listed, both artworks
  downloaded, one playing, listed once; a fresh listing keeps what was
  fetched; a 404 and an artwork with no address are final; a rate-limited
  listing keeps the channel playing and waits 15 minutes, a missing key an
  hour, and a settings change lists again by itself; a web page is not
  kept; nothing is listed or downloaded while internal memory is low, and
  both resume; fresh listings on request through the API; eviction deletes
  the oldest, spares the kept and the young, and an artwork that went is
  fetched again when played; after a restart the channel plays from its
  index with nothing downloaded, and plays on when the listing fails.
  Suite: the suite without its slow tests on an earlier build of this work: 278 of 279 passed, the failure being the test hooks' screenshot, since fixed; on the later build the smoke, content, overlay, UI drawing, library and scheduler tests pass.
- Not verified on the device: eviction starting by itself on a full card
  (the dev card is 32 GB and nearly empty; the marks are host-tested and
  the walk is tested through a hook); a download given up for a playset
  change or a USB session; the 15-minute stop after a source refuses a
  download; the half-minute save after downloads (the restart test lists
  again to force a save); the network going away and coming back; stale
  indices dropped after 60 days; the new words on the screen (host-tested
  only). Frame timing while downloading is measured at the soak (M6.1).
- The test hooks' screenshot now copies the framebuffer before sending
  it. It used to read rows as it sent them, over seconds, so the bottom of
  the picture was a later frame than the top; `test_overlays.py`'s red
  mark (three seconds long) was missed whenever an animated artwork was on
  the panel. After the fix that test passed 11 runs of 12; the one failure
  was not caught and is not diagnosed.
- `http_fetch` logs a URL only up to its query: Giphy's and Klipy's keys
  are query parameters. Compiled, not run.
- O9 is half answered: transfers are serial by design. Packet mode, the
  Option E items and the thresholds need real traffic and move to M4.2.

### 2026-11-23: M4.1 part 1, the fetcher

- `legacy/components/http_fetch` moved to `components/p3a_fetch` (layer
  `library`: below the sources, which list through it). It was already
  the one HTTP client of 1.x, with the retry policy, redirects,
  `Retry-After` and the cap on concurrent TLS sessions; none of that
  changed.
- Its answers now use `p3a_err.h`: `MISSING` for 404, `RATE_LIMITED` for
  429, `AUTH` for 401 and 403, and `SD_FAILED` without trying when the
  card has failed or is shared over USB.
- **A download is hashed as it is written** (`hash_file`), and
  `p3a_blob_adopt_hashed()` takes such a file into the blob store without
  reading it again.
- It does not ask whether the network is up: the network is above it.
  Callers start on `P3A_EVENT_NET_UP`; a request with no network fails.
- D63: listing and downloading will be one task doing one transfer at a
  time, beside the scheduler (which owns the indices), not two tasks in
  the library layer as the design drew it.
- Verified, device tests against real servers (3 new cases in
  `test_fetch.py`): a page into memory; a file onto the card whose hash
  made on the way equals the one computed on the workstation, taken into
  the blob store under that name, and not stored twice; a 404 final after
  one attempt; an unreachable host tried three times; nothing left in the
  store after a failed download. With the smoke and library tests: 15
  cases.
- Not verified: `RATE_LIMITED` and `AUTH` answers against a server (they
  come with Giphy's tests, M4.2); redirects followed by hand; a download
  cut off half way; behaviour under low internal memory.

### 2026-11-23: M3.6, the SD card as a USB drive. Phase 3 built

- New component `p3a_usb` (layer `ui`: a session takes the card away from
  everything below it). TinyUSB as 1.x had it, a serial interface plus
  mass storage, without the PICO-8 vendor interface; the descriptors and
  `tusb_config.h` moved from `legacy/main`. `esp_tinyusb` 1.7.6~2 and
  `tinyusb` 0.17.0~2 are pinned to what 1.x shipped.
- **The session's rules are pure** (`usb_session.c`): a session begins
  when a computer configures the device and always ends in a restart; a
  mount within 1.5 s of an unmount is a suspected bounce and is looked at
  again; an ended session has 2 s to come back; a suspend that lasts ends
  it too. 1.x had the same rules as three timers, two worker tasks and
  five flags in `app_usb.c`.
- **Taking the card**: `play_scheduler_shutdown()`, then
  `p3a_storage_hand_over()`, after which every write through the storage
  layer is refused until the restart; the computer is told "no medium"
  until both are done. `P3A_SCREEN_USB` says the card is in use, and then
  that the device is restarting.
- Uploads, next and back answer 409 `CARD_IN_USE` meanwhile (1.x refused
  uploads only).
- The configuration guard lists one new difference from 1.x, the vendor
  interface.
- Verified, host tests: 30 programs, also under the sanitizers; new is
  `test_usb_session` (5 cases).
- Verified, device test (`test_usb.py`, the bus's reports injected through
  `!ui usb`): the card is taken, playback stops, the screen says so,
  uploads, next and a playset save are refused, gestures do nothing, and
  two seconds after the computer lets go the device announces the
  restart, restarts, and has its card back in service. with it every test not marked slow passes: 267 cases.
- Verified, builds: release, test and trace with the build checks and the
  configuration guard; a from-scratch configure in the CI container, since
  a managed component was added.
- **Not verified: anything a real computer does.** No sector was served
  to a host, on any operating system. That is manual check M3.6-a, and
  until it is done this milestone is built, not done.

### 2026-11-23: M3.5, the overlays

- **The processing indicator**: the checkered triangle in the viewer's
  bottom right corner, blue while what a person asked for is loading, red
  for three seconds when it could not be shown. Its state follows the
  scheduler (a new `asked` in its status); nothing "starts" or "clears"
  it, where 1.x had a start call, a success call, a failure call and a
  30 second watchdog for the paths that forgot. Settings
  `indicator_enabled` and `indicator_size` as before.
- **The clock** (idea 1), off by default: hours and minutes over the
  artwork, in one of nine places, with a size, a colour and an outline
  that keeps it legible over anything. Hidden until the device knows the
  time, and under any screen. Eight settings in a new `clock` group, one
  of them `timezone`, a POSIX TZ string (D62; how a person chooses it is
  O19). A still artwork is redrawn when the minute turns.
- **Corner icons**: like, unlike, click, pin, unpin and error, flashed
  for four seconds top right or top left through `p3a_ui_icon_show()`.
  The six pictures are 1.x's, taken out of 4,700 lines of hex arrays into
  PNGs and small picture files (`ui_image.c`, `tools/gen_image.py`),
  which closes finding 24. Their real triggers come with the sources;
  for now a swipe on an artwork answers with the error icon, as 1.x did
  for a file of the user's own.
- The legacy overlay files and icon arrays are deleted (deletion log).
- **Source branding (idea 2) moves to M4.2**, where Giphy and Klipy and
  their terms are (O6).
- Verified, host tests: 29 programs, also under the sanitizers; new are
  `test_ui_clock` (3 cases), `test_ui_image` (3, with 20,000 damaged
  files) and cases for the indicator's state and drawing.
- Verified, device tests (3 new cases in `test_overlays.py`, against the
  workstation's rendering): the clock in two zones, both hour formats,
  two places and sizes, redrawn when the minute turns, and gone when
  turned off; the error icon after a swipe, gone after four seconds from
  a still artwork; the red indicator after a failed request, over the
  notice's band. With the smoke, settings and UI tests: 28 cases.
- Not verified: the overlays by eye (M3.5-a); the blue indicator on the
  panel (it shows for the fraction of a second a local file takes to
  load; its state is host-tested); frame timing with the clock up.

### 2026-11-23: M3.3 part 3, the SD card on the screen

- **Formatting the card on the device** is back, as a flow of the model's
  new `P3A_SCREEN_CARD`. The steps are `ui_card.c` (pure): the offer, a
  look at the slot, the warning whose confirm button is dead for three
  seconds, the format, the restart. p3a still never formats unasked.
  - At boot with no usable card the screen says "No usable SD card" and
    offers the format. 2.0 boots on regardless (H10); 1.x stopped there.
  - On the information screen a "Format SD card" button appears once the
    card in use has stopped taking writes.
  - The work is `p3a_storage_format()` over the new
    `p3a_board_sd_format()` (1.x's two paths: a mounted card, and one that
    would not mount). `p3a_storage_card_state()` tells an absent card
    from one with no usable file system, by the mount's own error.
- **Screens can have buttons**: up to two labelled targets, 90 pixels
  high, drawn and hit-tested by the same arithmetic (`ui_draw.c`). A
  dimmed button takes the tap and does nothing. `GET /api/v2/ui` lists
  them with their rectangles.
- **`play_scheduler_shutdown()`**: playback stops for good until the
  restart and nothing more is asked of the card. Used before the format;
  USB mass storage (M3.6) needs the same.
- The notice that the card stopped working, and the one that it was
  repaired at boot, are shown once for 20 seconds, as in 1.x.
- A touch controller that does not answer is said on the screen with a
  ten second countdown before the one restart; if the restart does not
  help, a notice says to use the web page.
- `GET /api/v2/ui/intro-animations` lists the 22 names.
- Verified, host tests: 27 programs, also under the sanitizers; new is
  `test_ui_card` (6 cases), and cases for buttons in the route.
- Verified, device tests with scripted fingers (`test_card_flow.py`): the
  offer on the information screen after the card is latched as failed,
  the dead confirm button, cancelling by button and by long press with
  the card untouched, and **a real format of the dev unit's card**
  followed by the restart into an empty card in service. 278 cases passed and 6 timing cases were skipped; 3 failed in the touch tests, whose expectation of a tap on the information screen was out of date, and pass since.
- Not verified: the boot-time offer and the "no card found" and "the card
  works" answers (they need the card taken out: manual check M3.3-a); the
  format of a card that would not mount (the same); a format that fails;
  the repair notice and the touch countdown (they need a failing card and
  a failing controller); buttons at rotations other than 0 on the device
  (host-tested through the canvas).

### 2026-11-23: M3.3 parts 1 and 2, the boot animation, information and setup

- **The 22 boot animations and the logo moved** from
  `legacy/components/p3a_core` to `components/p3a_ui/intro/` unchanged
  but for one thing: their scratch buffer is PSRAM on the device. The
  logo stays a C array, because the workstation harness compiles the same
  files; finding 24 (hex image arrays) is about the icons, which arrive
  with their overlays.
- `ui_intro.c` is 1.x's `p3a_boot_logo.c` on 2.0's settings
  (`intro_animation`, `intro_duration_ms`): 250 ms of background, the
  animation, the logo held for a second, timed from the first frame drawn.
- **The boot animation is a screen of the model** (`P3A_SCREEN_INTRO`,
  just above a notice). The UI now starts with the display, so the
  animation covers the rest of the boot; the first artwork loads under it
  and is shown when it ends. Gestures do nothing meanwhile.
- `host/intro-anim-lab` builds from the new place; its `--check` passes.
- **The information screen** is rows of label and value: firmware,
  uptime, Wi-Fi, signal, address, web page, card, playset. `app_main`
  fills it (the network is above the UI) and it is refreshed every two
  seconds while up. Makapix and Giphy rows come with those sources.
- **The setup screen** is put up by the network when it opens its access
  point, with the network's name and the address to open.
- A screen's text may now mix paragraphs and rows (a line with a tab is a
  row) and has a hint line at the bottom ("Long press to close"). The
  fatal screen's title is red.
- **A fault found by the wider run and fixed:** the UI took its hold on
  playback before the scheduler existed and released it afterwards, which
  released the test build's hold instead. Holds taken before the
  scheduler starts are now applied when it starts.
- Verified, host tests: 26 programs, also under the sanitizers; the lab's
  contract checks for all 22 animations.
- Verified, device tests: 3 new cases (the chosen animation plays, ends
  on the logo with every frame inside its 33 ms budget and hands over to
  the artwork; an unknown name falls back to a random one; rows and a
  hint equal the workstation's rendering). With the smoke, API, pipeline,
  UI, drawing and touch tests: 45 cases on the final firmware. Before the
  information screen and the hold fix, every test not marked slow ran:
  260 passed and 2 failed, both since fixed (the hold, and a test that
  named the first job of a boot).
- Not verified: each of the 22 animations by eye on the panel in 2.0
  (they are unchanged, and were device-verified in 1.x); the setup screen
  through a real setup (it needs the unit taken off Wi-Fi: part of manual
  check M1.5-a).

### 2026-11-23: uGFX is dropped (O3 decided)

- Fab agreed to drop uGFX after the trade-offs were laid out (D60): 2.0
  draws text with its own renderer and gives up anti-aliased text and a
  ready-made widget toolkit, neither of which the plan uses.
- His observation, kept for later: characters outside Latin may be wanted
  at some point. Recorded as idea 5 in
  `docs/2.0-architecture-review/ideas.md` with what it would take.
- `legacy/components/ugfx` and `legacy/main/ugfx_ui.c` stay in the quarry
  until M3.3 has taken the screens' content from them; their deletion is
  logged then.

### 2026-11-23: the font is Everyday Ample (O4 decided)

- Fab chose Everyday Ample from the specimen sheet (D61).
  `fonts/ui.p3afont` is regenerated from its TTF at 9 px: 157 characters,
  3 KB. One family for every role; the scales stay 4, 3 and 2 for title,
  text and band, which gives glyphs 36, 27 and 18 pixels high.
- O3 stands as decided in D60 unless Fab says otherwise: he was asked and
  answered the font question only.
- Verified: host tests (26 programs); on the dev unit the drawing, UI and
  touch tests, which compare the panel with the workstation's rendering
  of the new font.
- Not verified: the look by eye on the panel (M3.2-a stays open for that).

### 2026-11-23: M3.4 part 2, the touch task and what gestures do

- `ui_route.c` (pure): one function from the screen on top, the pause
  state and a gesture to an action. On the artwork: tap right or left is
  next or back, two fingers turned turn the screen, a long press opens the
  information screen. Paused: only a long press, which resumes. The
  information, setup and registration screens: a long press closes them.
  An update, a shared card and a fatal error: nothing.
- `ui_touch.c`: the touch task (class `TOUCH`) reads the controller every
  20 ms, feeds the recogniser, asks the table and acts: the scheduler for
  next, back and resume, the `rotation` setting for a turn (saved, as in
  1.x), the model for the information screen.
- **The information screen** has first content: the device's name, its
  address, the firmware version and the card's free space, filled by
  `app_main` because the network is above the UI. Its full content is
  M3.3.
- **Scripted fingers** (`p3a_ui_touch_script()`, `!ui touch`): the task
  reads a script in place of the controller at the same pace, so a test
  takes the path real fingers take.
- Swipe up and down are recognised and do nothing yet: what they do
  depends on where the artwork is from (Phase 4).
- Checked on the way: the UI's rotation agrees with the artworks' (the
  canvas and `test_player.TURN` put the same corner in the same place),
  so a turned screen's "right half" is where the viewer sees it.
- Verified, host tests: 26 programs, also under the sanitizers; new is
  `test_ui_route` (5 cases).
- Verified, device tests (6 new cases in `test_touch.py`, scripted
  fingers): tap right and left, a drift that does nothing, both swipes
  recognised, long press opening and closing the information screen with
  a tap under it ignored, pause, two-finger turns both ways with the
  setting saved and taps following the turned screen, an update not
  interrupted. With the smoke, UI, drawing, pipeline, player and board
  display tests: 71 cases pass.
- Not verified: real fingers (manual check M3.4-a); the task's cost while
  an animation plays (to measure with the trace build).

### 2026-11-23: M3.4 part 1, the gesture recogniser

- `ui_gesture.c` (pure): one touch sample in, at most one gesture out.
  Tap (reported on release, placed as the viewer sees a turned screen),
  swipe up and down (a quarter of the screen, as the viewer sees it), long
  press (2.25 s, still within 6.5% of the screen), two-finger rotate (45
  degrees, once per gesture). The thresholds and the order of the checks
  are those of 1.x's `app_touch.c`, which were tuned with real fingers.
- One difference from 1.x: when two fingers are lifted one after the
  other, the finger left on the panel no longer ends as a tap.
- A touch that drifts further than a tap may and less than a swipe needs
  is nothing, as in 1.x.
- Nothing feeds it yet: the touch task and what each gesture does per
  screen are the rest of M3.4.
- Verified, host tests: `test_ui_gesture` (8 cases), with the others (25
  programs), also under the sanitizers. Not on the device: it is not
  wired.

### 2026-11-23: M3.2, text on the panel (provisional font)

- **O3, working answer: uGFX is not carried over.** Text is drawn by
  `ui_font.c` (pure, about 200 lines): a pixel font at whole multiples of
  its own size, UTF-8, wrapping and alignment. Fab may still overrule;
  nothing else depends on uGFX.
- **Fonts are small binary files** made from a pixel font's TTF by
  `tools/gen_font.py` (1 bit a pixel, no anti-aliasing) and committed, so a
  build needs neither Pillow nor the TTFs. Every size in the file is
  checked when it is opened. The built-in one is `fonts/ui.p3afont`:
  Everyday Standard by VEXED, 6 px, 159 characters (ASCII and the Latin-1
  letters), 3 KB, CC BY 4.0 with the attribution in `fonts/README.md`.
  **The family is provisional (O4).**
- **`ui_canvas.c`** (pure): everything is drawn through one fill that
  turns viewer coordinates into the panel's for the rotation in force and
  clips. No drawing code knows about rotation.
- **`ui_draw.c`** (pure): one layout for every screen that is words (a
  title, a paragraph, a progress bar when there is one, centred as a
  group) and a band along the bottom for a notice over an artwork. The
  look is the few numbers of `ui_style_default()`.
- **`ui_display.c`**: the model drawn as the display pipeline's UI source
  and as an overlay. The UI takes the panel when a screen covers the
  artwork and gives it back, through a job. A still artwork is redrawn
  when a banner appears or goes (`display_renderer_request_redraw()`, one
  flag beside the FPS counter's; the timing code is untouched).
- So the notices of M3.1 are now seen: "Nothing to play" with the reason
  on a device with an empty card, a failed request as a band over what
  plays, the co-processor update with its progress bar.
- `tests/host/ui/ui_preview.c` draws any screen on the workstation with
  the device's code; the device tests compare the panel with it.
- Verified, host tests: 24 programs pass, also under ASan and UBSan in
  WSL. New: `test_ui_text` (8 cases: rotation and clipping of the canvas,
  measuring, drawing, UTF-8 including malformed, wrapping, a font file
  refused in nine ways, and 20,000 damaged font files that are refused or
  harmless) and `test_ui_draw` (4: order and centring, margins, the bar's
  fill, nothing pushed off the screen by any words at any rotation).
- Verified, device tests (5 new cases in `test_ui_drawing.py`): the panel
  equals the workstation's rendering pixel for pixel for a screen at
  rotations 0, 90, 180 and 270, after a change of words, and for a band
  over a still artwork; the artwork returns untouched. With them the smoke, UI, player, pipeline, scheduler and player-API tests pass (62 cases) on the final firmware; the other tests not marked slow passed (190 cases) on the build just before the last two fixes.
- **A stall found by the wider test run and fixed.** With a notice
  covering the panel, a newly loaded artwork was never drawn, so it was
  never reported shown, and the notice never left. The scheduler's status
  now says when a request is decoded and waiting for the display
  (`staged`), and the UI gives the panel up for it. The same for a
  request to play nothing.
- Not verified: the look, by eye, on the panel (manual check M3.2-a); a
  banner over an animation (the same overlay path as the FPS counter);
  frame timing with a banner up (to measure with the trace build).
- Still to do in M3.2: Fab's choice of family and sizes; image assets
  through `EMBED_FILES` (they arrive with the screens that need them).

### 2026-11-23: M3.1, the screen model and the hook table

- **O7 decided (D59): one model in the UI, and no `p3a_state`.** New
  component `p3a_ui` (layer `ui`). `ui_model.c` (pure) holds every screen
  and notice that is up; what is seen follows by fixed precedence, so a
  screen coming down uncovers what is beneath it and writers cannot fight.
  The 1.x state machine, its render state with the four-writer
  arbitration, the UI mode and the display's mode flags are not carried
  over; playback's own state is the scheduler's.
- **Notices follow the facts.** `ui_player.c` (pure) works out "getting
  ready" and "nothing to play, and why" from the scheduler's state, and
  puts a failed request in plain words. Nobody calls "show loading". The
  scheduler emits one new event, `P3A_EVENT_PLAYER_CHANGED`, and the UI
  looks. "Getting ready" is held back for 1.5 seconds so that it does not
  flash by at an ordinary boot.
- **A notice is a banner while an artwork plays**, and the whole screen
  only when nothing does. In 1.x every channel message took the screen
  from the artwork.
- **The hook table** is `p3a_ui_hooks.h` in `p3a_util`: four calls any
  layer may make, which do nothing until `app_main` hands over the UI's
  functions. The co-processor update reports through it. The UI learns
  about the network's setup mode and a failed card from their events.
- A screen over the artwork holds the dwell timer; a banner does not.
- Notices may expire and may be capped per boot (the 1.x rule for
  rate-limit messages: 20 seconds, twice per boot).
- `GET /api/v2/ui` says what is on the screen. `!ui` puts screens and
  notices up for tests.
- Audit M25 (a message type passed as a bare `int`) and M27 (fields
  written and never read) are gone with the code that had them.
- Verified, host tests: 22 programs pass, also under ASan and UBSan in
  WSL; new are `test_ui_model` (10 cases) and `test_ui_player` (4).
- Verified, device tests (4 new cases in `test_ui.py`): the notices
  following the player from an empty folder to a playing file to a failed
  request, precedence and uncovering, a screen holding playback while a
  banner does not, the per-boot cap. With them the smoke, pipeline and
  player tests: 48 cases pass.
- Not verified: anything on the panel (nothing is drawn until M3.2); the
  setup screen and the failed-card notice through their real events; the
  co-processor update's progress through the hooks.

### 2026-11-23: M2.4, the loader pipeline, changes C and D. Phase 2 complete

- **The rules of a turn** are pure code, `sched_turn.c`: when the device
  moves on by itself, and what a failure leads to depending on who asked
  (D55). The device's own turns and "next" try another artwork, up to three
  times; a request for one artwork (back, forward, "show this") does not.
  What a person asked for and did not get is kept as `problem` in the
  player's state until something is shown; the UI shows it from M3.1.
- **The dwell timer.** `dwell_s` seconds after an artwork reaches the
  panel, the next one is asked for. Not while paused, while a screen holds
  it (`play_scheduler_hold()`, for M3), while something is loading, or
  when there is nothing else to show. A change of the setting applies to
  the artwork on the panel. A device whose first picks all failed tries
  again after a dwell instead of staying black.
- **Failures by error.** Missing, damaged and unplayable artworks are set
  aside until their channel is listed again. A damaged blob is deleted
  through the blob store, at most one a minute; a user's own file never is.
- **Pause**: black panel, backlight off, no moving on; resume restores the
  brightness setting and gives the artwork a full dwell. The BOOT button
  toggles it (watched after the co-processor update, as in 1.x). Next,
  back and uploads-that-play are refused while paused (D58). A brightness
  setting now takes effect at once (it was applied only at boot).
- **"Show this"**: `play_scheduler_show()` plays one record now and then
  carries on with the playset. An uploaded file plays at once through it.
- **Change C, less repetition** (`sched_pick.c`, D56). The last 256
  artworks shown are remembered in memory. A random pick accepts a
  candidate with a chance that grows with the square of how long ago it
  was shown, never below 1 in 16, and never picks the artwork shown last
  while the channel has another. Measured in the host test: an artwork
  comes back within a quarter of its channel's length 1.5% to 4.3% of the
  time, against 22% for the plain random pick, and every artwork keeps its
  share.
- **Change D, the aspect filter** (D57). The predicate and its two
  settings (`aspect_filter`, `aspect_max_tenths`, default 4:1). A source
  asks for it in its table (`filter_aspect`); none does yet. The player now
  tells its listener the picture's size at "ready", before the artwork can
  reach the panel, and the listener may say no: that is how an artwork of
  unknown size is measured, remembered and passed over without counting as
  a failure.
- **O10, the decode budget, moves to M4.3.** In the new pipeline a slow
  decode holds nothing up but the next artwork: the previous one keeps
  playing and the timer waits. Sizing a budget needs the slow museum
  JPEGs, which arrive with the museum source.
- **O18 is still open** (smooth or sharp scaling for the user's files);
  2.0 does what 1.x did, sharp.
- A test build holds the device still from boot (`sched auto off`), so
  that tests find on the panel what they put there.
- `!button` now reads a count the board keeps; `app_main` owns the button.
- Verified, host tests: 20 programs pass, also under ASan and UBSan in
  WSL; new are `test_sched_turn` (8 cases) and `test_sched_pick` (8).
- Verified, device tests (5 new cases in `test_pipeline.py`): moving on
  after the dwell and never to the same artwork, a dwell of 0, pause and
  resume with the backlight, a damaged file passed over by "next" and left
  on the card, a named artwork that fails reported with nothing put in its
  place, an upload playing at once and joining the history.
- Verified, end of milestone: the whole device suite (260 cases pass; the 6 timing cases need the trace build and were not run), and the
  release and trace builds with the build checks and the configuration
  guard.
- Not verified: deleting a damaged blob and the aspect filter on the
  device (no source makes blobs or asks for the filter until Phase 4); the
  BOOT button (manual check M2.4-a); a decode that fails in the middle of
  an animation; pause while a file is loading.
- Not carried over from 1.x, on purpose: the on-screen "Playback Error"
  (the UI's, M3.3, from `problem`), the LRU touch of a played file
  (eviction, M4.1), the view tracker and current-post calls (M4.5).

### 2026-11-23: M2.3 part 5, the routes; M2.3 done

- `components/p3a_net/net_player_api.c`: `GET /api/v2/player`, `POST
  /player/next` and `/player/back`, `GET|PUT /player/playset` (the active
  one; a whole playset or `{"saved": name}`), `GET /playsets`,
  `GET|PUT|DELETE /playsets/{name}`, and `POST /files?name=` (upload).
- **No separate `player_commands` module.** The plan named one for HTTP
  and Makapix to share. The scheduler's public functions are that surface
  already (activate, next, back, refresh), and the routes are thin calls
  to them; a second layer would only forward. Makapix commands (M4.5) call
  the same functions.
- **Upload takes the file as the request body** with its name in the
  query, not a multipart form. The new web UI sends a file that way as
  easily, and 250 lines of multipart parsing are not carried over. It is
  written beside its final name and moved into place when complete, so a
  broken upload leaves no half file among the user's pictures. Checked
  first: the name (unchanged by the card's rules, at most 119 characters,
  a picture's extension), the size (16 MiB) and the card's free space.
- A name the card cannot hold is refused, where 1.x silently changed it.
  That also settles audit M7: the answer carries the name through the JSON
  builder, and the characters that would need escaping cannot be in a name.
- Differences from 1.x to come back to: an uploaded file is listed but
  does not play at once (needs "show this file", M2.4); uploads are not
  yet refused while the card is shared over USB (no USB sharing in 2.0
  yet); the active playset has no ETag (M5.1, if the UI polls it).
- Errors say what to do: `NOTHING_TO_PLAY`, `TOO_MANY_PLAYSETS`,
  `UNREADABLE_PLAYSET`, `INVALID_PLAYSET` with the parser's sentence,
  `CARD_FULL`, `UNSUPPORTED_TYPE`.
- JSON bodies may now be 32 KB (a 64-channel playset is about 20 KB).
- Verified, device tests (5 new cases): upload, listing and playing of an
  uploaded file, replacing by the same name, seven refused uploads, next
  and back to the far end of the history, playsets saved, read, sent back,
  refused and deleted, the active playset set whole and by a saved name.
- Verified, end of milestone: the whole device suite (255 cases; one needed its own bound updated for the larger JSON body limit), and the
  release and trace builds with the build checks and the configuration
  guard.
- Not verified: an upload near 16 MiB (time and memory), an upload cut
  off half way, and two uploads at once.

### 2026-11-23: M2.3 part 4, the scheduler proper

- `play_scheduler.c`: the active playset's channels are listed through
  their sources in one background job, into an index each; "next" chooses
  a channel and an artwork (`sched_core`), finds its bytes (the source's
  `path` for a record in place, the blob store otherwise) and hands them
  to the animation player; the player's reports close the turn.
- **History is written on "shown" only** (finding 7). A request that fails
  takes its channel's turn back, enters no history, and leaves the artwork
  that was playing on the panel. A file that turns out to be missing is no
  longer offered until its channel is listed again. 1.x committed history
  at pick time.
- `sched_history.c` (pure): the last 32 artworks shown, as whole records,
  so "back" still works after the playset changes. "Next" after "back"
  walks forward again before anything new is picked.
- **The device now plays by itself at boot**: the active playset is read
  from the card and its first artwork is chosen when its first channel is
  listed. With no playset saved, the device plays the user's own files
  (D53). Nothing moves on by itself yet: the dwell timer is M2.4.
- A channel whose source this firmware does not have is passed over, not
  an error: a playset saved with Giphy channels plays its other channels
  until M4.2 brings Giphy.
- Limits, for now: at most 16 of a playset's channels are loaded (the
  rest are marked "not loaded" and never chosen; loading on demand comes
  with the remote sources), and a listing replaces the channel's index
  whole.
- The scheduler is the animation player's listener. The test hook that
  plays files directly gets its reports passed on
  (`play_scheduler_set_foreign_listener`): tokens with the top bit set are
  the scheduler's.
- Learned from a test: a GIF cut off half way loads and shows its first
  frame; it fails only when the missing frames are reached, and is then
  held on its last good frame. That is the player's intended latch, seen
  on the device for the first time, though still without a test that
  asserts it.
- Verified, host tests (18 programs, also under the sanitizers in WSL; 3
  new cases for the history).
- Verified, device tests: the scheduler's 4 cases (listing, a full round
  in channel order, back and forward, three vanished files and a damaged
  one, a playset with an absent source, resuming after a restart), and
  every device test not marked slow, since the boot sequence changed.
- Not verified: two or more channels with weights on the device (only the
  local source exists; the shares are host-tested); a playset of more than
  16 channels; what happens when a listing and a "next" race (the lock
  covers it by design, no test provokes it).

### 2026-11-23: M2.3 part 3, the source interface and the local source

- **The layer order changed: `library` is now below `sources`** (D51). The
  review's diagram had sources underneath, yet a source has to produce
  records and fill an index, which are the library's. So the library
  defines the interface (`p3a_source.h`: a table of functions and a
  registry) and the sources sit above it, implement the table and register
  at boot. The library's later managers (downloads, refresh) call through
  the registry and still never include a source. `tools/check_tree.py`,
  `LAYERS.txt`, `AGENTS.md` and `PLAN.md` say the new order.
- `p3a_source_ops_t` has what is needed so far: `refresh` (list a channel
  into an index), `path` (for records whose bytes are not blobs), the
  refresh interval, and the scaling policy. `url` and the rest arrive with
  the first remote source (M4.2).
- New record flag `P3A_RECORD_IN_PLACE`: the bytes are where the source
  keeps them. Such a record is available without a blob.
- New component `components/source_local` (layer `sources`): lists the
  user's `animations/` folder, newest first, files left where they are
  (working decision from M2.2, now D52). Files are chosen by extension;
  hidden files and macOS "._" companions are passed over. A picture whose
  name is over 119 characters, or beyond the index's capacity, is counted
  and left out with one warning, never cut. Registered at boot.
- Found by its test: a long-named file in a full index was counted as
  "over capacity" instead of "name too long". Fixed.
- The local source scales with sharp pixels, as 1.x did for the card's
  files. A user's photographs would look better smooth; left for Fab to
  decide (O18).
- The device test removed the two 1.2.5 check pictures from the card's
  `animations/` folder; copies remain on the workstation.
- Verified, host tests (17 programs, also under the sanitizers in WSL; 3
  new cases): the registry, records in place, which file names are
  pictures.
- Verified, device tests (smoke and library: 12 cases): the folder listed
  through the registered table with pictures, a text file, a hidden file
  and an over-long name; every listed file available and found at its
  path; an index smaller than the folder.
- Not verified: a folder of thousands of files (scan time unknown); file
  names with characters outside ASCII.

### 2026-11-23: M2.3 part 2, playsets

- `playset.c` (pure): the playset has one form. A 224-byte packed channel
  (source, kind, identifier of up to 95 characters, display name, weight,
  offset) is what is held in memory and written to the card; JSON is its
  form over the API. 1.x had three forms, one of them a raw dump of an
  860-byte in-memory struct, and 32-character identifiers.
- **JSON is checked strictly**: unknown keys, wrong types, fractions,
  negative numbers and text that does not fit are refused with a sentence
  saying which. 1.x cut long text silently and ignored typos. The reason
  goes back to the caller for the API to show.
- The 1.x channel "type" (named, user, hashtag, reactions, sdcard, ...)
  becomes `source` plus `kind`: Makapix's four types are four kinds of one
  source. This is a new JSON shape; the web UI is rewritten for it in
  Phase 5, and commands arriving from Makapix are translated in M4.5.
- The single "show this artwork" pseudo-channel and its 716-byte
  sub-struct are gone from the playset; it returns as a command of its own.
- The channel id is 16 hex digits of FNV-1a over source, kind, identifier
  and offset in a canonical byte form; weight and display name do not
  change it. One value is pinned in a test, since ids name files.
- `playset_store.c`: saved playsets and the active playset are framed
  containers (schema 3) flagged as user data, in `playsets/`. Files are
  named by a hash of the playset's name, so names may differ by case or
  hold characters FAT refuses. A file that cannot be read is counted in
  the listing and never overwritten by a save; only the user's delete
  removes it. At most 32 are saved. Closes audit M21.
- Verified, host tests (17 programs, also under the sanitizers in WSL; 6
  new cases): layout, JSON round trip, the exact length limits, 19 wrong
  shapes each refused with a reason, the channel id, and every single-byte
  change to a packed playset either reading as sound or being refused.
- Verified, device tests (smoke and playsets: 11 cases): save, list,
  replace, names differing by case, delete; a damaged file reported, not
  listed, not overwritten; the limit of 32; the active playset across a
  restart.
- Not verified: a playset as large as 64 channels on the device (the test
  console's line limit is 1 KB; the HTTP route will carry large ones).

### 2026-11-23: manual check M2.1-a cleared

Run by the agent with Fab at the panel, one picture at a time, on the test
build.

| What was shown | What Fab saw |
|---|---|
| A generated 64 px pixel-art scene, 20 fps, nearest neighbour | Smooth motion, no stutter or tearing; clean square pixels |
| `images/pico-8/pico-8-gameplay.webp` (500 px, 250 frames) | Picture correct; plays slower than the file asks (see below) |
| A wide museum JPEG (1263 by 745), smooth scaler | Natural tones; centred with even bands |
| A transparent PNG over a dark blue background colour | Uniform blue, the soft glow fades straight into it |
| The same JPEG, then rotation set to 90 | Turned a quarter at once, looks as good |

- **Slow heavy WebP, measured.** The PICO-8 file asks for 10 fps and the
  unit shows about 5.7: decoding one 500 px lossy frame with alpha takes
  140 to 270 ms in libwebp (measured through the decode hook). Every frame
  is shown; none is dropped. The WebP component is byte-identical to
  `main`'s and the optimisation settings are the same, so 1.x should be as
  slow, but 1.x was not measured on this file.
- **Fab restated a design rule: p3a never drops frames.** A file that
  cannot be decoded in time plays slower. 2.0 keeps this (D49).
- A first transparent test picture showed a faint dark ring around the
  glow. The ring was in the file (its colour had been blurred together
  with its opacity). A corrected file showed none. No firmware change.
- Not covered by this look: Giphy-sized GIFs through the smooth scaler in
  motion, and long viewing. The soak (M6.1) is where rare stutter would
  show.

### 2026-11-23: the 1.2.5 picture fixes checked on a device

- The build of `main` (`9466c035`) ran on the dev unit: the 1.2.4 flash
  backup with the new app written into its active slot, as an OTA update
  would leave a unit in the field. The boot log confirmed the slot and the
  compile time.
- Two test pictures were made so that the answer is yes or no. A JPEG
  with words 17 levels inside white and 22 above black: the old conversion
  maps the first to pure white and the second to 7, so the words vanish.
  An animated WebP with words at 60% opacity on transparent: the old blend
  turns them into background. Both were first played on 2.0, whose
  decoders carry the same fixes, and read back from its framebuffer as
  expected.
- Fab looked at the panel under 1.x: both sets of JPEG words readable; the
  WebP words visible with the moving dot. The fixes work on 1.x.
- 2.0 was put back afterwards: flash erased, test build flashed, unit
  provisioned, smoke tests run. The temporary flash image and the
  temporary checkout of `main` are deleted; the 1.2.4 backup is untouched.
- Left behind: `check-jpeg.jpg` and `check-webp.webp` in the card's
  `animations/` folder (1.x has no call to delete an upload), and copies in
  `C:\Users\fab\p3a-1.2.5-check\`, useful for manual check M2.1-a.
- While on 1.x the unit was live on Makapix under its own registration for
  about ten minutes.
- `p3a.local` gave no answer to a name lookup on 2026-11-23 (the other
  unit that used it has left the LAN, Fab says). The lab still names the
  dev unit `fab`; nothing depends on the bare name being free.
- `main` records the check in `dbb7ec51`.

### 2026-11-19: M2.3 part 1, the scheduler's arithmetic

- New component `components/play_scheduler` (layer `player`) with one pure
  file, `sched_core.c`, written from `legacy/.../play_scheduler_swrr.c` and
  the six pick functions of `play_scheduler_pick.c`: choose a channel
  (round robin or weighted random, the two modes of the `channel_select`
  setting, with 1.x's formulas), choose an artwork in it (random or in
  channel order, the `pick_mode` setting), and undo a turn.
- The core knows a channel as a weight and a count of playable artworks,
  and answers with "the n-th playable artwork", which
  `p3a_index_nth_available()` turns into a record. So there is one pick
  for every source, where 1.x had one per record layout.
- **Undoing a turn** is how a pick that ends with nothing shown (finding 7)
  costs its channel nothing: the shares are restored exactly, while the
  channel's cursor and random sequence move on, so the next try is another
  artwork. 1.x let the charge stand.
- A channel that gets its first artwork starts level with the others; in
  1.x it kept whatever credit it had when it ran dry.
- The legacy files stay in `legacy/` for now: the rest of the scheduler
  (playsets, refresh, navigation, history) is still to be read out of them.
- Verified, host tests (16 programs; 9 new cases): exact 3:1 shares and
  even spacing in round robin; equal shares without weights; the random
  mode within 3% of the weights over 30,000 turns; channels with nothing
  to play, arriving and running dry; undo, with every third turn undone
  and the shares still exact; recency order and wrap; random picks in
  range and covering everything; same seed, same choices.
- Verified, build: it compiles for the device in the test build. Nothing
  on the device uses it yet, so no device test was run for this
  checkpoint.

### 2026-11-19: M2.2 done: index files and the blob store

- `p3a_index_save()` and `p3a_index_load()`: an index is one framed
  container (schema 2) of its records. A file with more records than the
  caller has room for, a failed checksum, or one record that fails its own
  check refuses the whole file and leaves the index empty; the file is
  never touched. The flags say whether it is a cache or a list the user
  made.
- The blob store, `p3a_blob`: `blobs/{d0}/{d1}/{32 hex digits}.{ext}`,
  named by the first 16 bytes of the SHA-256 of the content.
  `p3a_blob_adopt()` hashes a finished file, moves it into place (never a
  copy), and drops it instead when the store already holds those bytes.
  `p3a_blob_remove()` is the only code that deletes artwork bytes.
- **The plan's `refs` table is not built (D48).** The design gave every
  blob a reference count and a pinned flag in one table, but named no
  reader for the count: eviction goes by age, as in 1.x. What must be
  known is only which blobs the user pinned, a small set that the pin
  lists themselves define. So: no global table (nothing to tear, nothing
  to rebuild, no megabytes of PSRAM for a count per file on the card);
  the set of kept blobs arrives with the pin lists, and
  `p3a_blob_remove()` will consult it. A record whose blob was evicted is
  found out when it is played (`MISSING`) and marked then.
- The file is hashed by reading it back after it is written. Hashing while
  the bytes stream in from the network is an optimisation for M4.1, when
  the fetch code arrives.
- Open for M2.3: whether the user's own files in `animations/` are moved
  into the blob store or indexed where they are. Moving them would hide
  them from someone browsing the card over USB; indexing in place means a
  local record points at a filename, not a blob. The record has room for
  both.
- Verified, device tests (smoke, library, storage: 26 cases, then the
  whole suite at the end of the milestone): an index of 2, 500 and 4096
  records round-trips on the card with every record found again by its
  identity; the three kinds of bad file are refused and kept; a blob's
  name equals the SHA-256 prefix computed on the workstation; the same
  bytes twice are one file; an empty file and an unknown format are
  refused and the file left in place; removal, and removal of what is not
  there.
- Numbers: 4096 records (786 KB) save in 1.6 s and load in 0.4 s.
- Not verified: the store under a failing card (the health latch is tested
  on its own, not through an adoption); a multi-megabyte adoption.

### 2026-11-19: the two picture fixes ported to main (D47)

- Fab answered O17: yes. `main` has them as `9466c035`, pushed. A push to
  `main` starts nothing; only a published release does.
- Done in a temporary second checkout of `main` outside Dropbox, removed
  afterwards, so the `v2` tree was never switched.
- Verified: 1.x builds with them (2.46 MB image). The three functions were
  compared with `v2`'s and are byte-identical.
- Not verified: the 1.x build was not flashed. The dev unit carries 2.0,
  and 1.x has no way for a machine to see its panel, so a flash would only
  have shown that it does not crash. The check that matters is eyes on a
  1.x unit before 1.2.5; it is recorded under "Ports to main".
- A fresh checkout of `main` does not build as it is: the Makapix
  certificate includes (`components/makapix/certs/`) are untracked. Copies
  are in this workstation's `legacy/components/makapix/certs/`.

### 2026-11-19: M2.1 closed; M2.2 begun

- The end-of-milestone run (D42): the whole device suite on the test
  build, 234 of 235 passed. The one failure was a test's own bound: after
  exhausting internal memory in 2 KB chunks it required the largest free
  block to be under 2048 bytes, and it was exactly 2048, which does not fit
  a 2048-byte allocation either (the allocator needs its header). The
  bound is now "at most 2048"; rerun, it passes. Nothing in the firmware
  changed for it.
- The release and trace builds pass their checks and the configuration
  guard. The release image is 1.36 MB (it was 0.91 MB before the decoders
  and the player were linked in).
- M2.1 is done as far as machines can say. Manual check M2.1-a (how
  playback looks) is Fab's.
- M2.2 begun while that run had the device: `components/p3a_library` with
  `p3a_record` (the 192-byte record: one layout for every source, the
  source in the record, identifiers refused when they do not fit, a check
  for records read from a file) and `p3a_index` (records in channel order,
  an open-addressed table over the 32-bit key, an availability bitmap,
  "the n-th available record" for picks). Two artworks that share a 32-bit
  key are both kept and told apart by identity; 1.x left that collision
  unhandled.
- The slot table and the bitmap are rebuilt when an index is loaded, not
  stored: a few hundred microseconds, and two fewer things that can
  disagree with the records on disk.
- Verified, host tests (15 programs; the new one has 11 cases): layout and
  the numbers written to disk, every source's record, identifier limits at
  the exact boundary, damaged records, a full 4096-record index, shared
  keys, removal, availability.
- Not verified: nothing of M2.2 has run on the device or under the
  sanitizers yet (CI runs them on the next push).

### 2026-11-19: M2.1 part 4, the frame trace and a first timing baseline

- **The frame trace was never started in 2.0.** The trace build compiled it
  in, and nothing called `frame_trace_init()`. `app_main` does now. Found
  only because part 4 tried to read it.
- The trace's read-out is in `components/p3a_net/net_trace_api.c` (from the
  1.x file, moved), under `/api/v2/debug/`, in builds that trace. The CSV
  is the 1.x format, so `host/jitter-lab/pull_frames.py` and `analyze.py`
  work against 2.0 with their URLs changed; a pull and an analysis were
  run against the dev unit. The lab's other tools still speak to 1.x
  routes and are adapted with the soak (M6.1); its README says which.
- New lab build `trace-test`: the frame trace with the test hooks, so that
  a test can play files and read their timing.
  `python -m pytest tests/device/test_timing.py --build trace-test -s`.
- **First timing readings of 2.0** (dev unit, 20 s each, rotation 0):

  | Playing | Frames | Late p50 | Late p99 | Late max | Produce p50 |
  |---|---|---|---|---|---|
  | 64 px GIF, 10 fps, nearest | 202 | 0.2 ms | 8.3 ms | 8.5 ms | 15 ms |
  | 240 px GIF, 25 fps, nearest | 505 | 0.1 ms | 8.4 ms | 8.4 ms | 28 ms |
  | 240 px lossless WebP, 12 fps, nearest | 251 | 0.4 ms | 8.4 ms | 8.4 ms | 47 ms |
  | 240 px GIF, 25 fps, smooth (hardware scaler) | 505 | 0.0 ms | 8.4 ms | 8.4 ms | 18 ms |
  | the 240 px GIF while 8 loads fail | 381 (15 s) | 0.1 ms | 8.4 ms | 8.6 ms | 28 ms |
  | 6 swaps between a 700 px PNG and the GIF | 455 | 0.1 ms | 8.4 ms | 8.5 ms | 29 ms |

  No frame was late by more than about half a refresh (the vsync lock's
  grain), including while the next artwork loaded and across swaps. This is
  a baseline of minutes on synthetic files. It is **not** the jitter pass
  bar, which is hours of real use; that is M6.1.
- A capacity fact, not a regression as far as can be told: a full-frame
  lossless WebP animation at 240 px takes about 47 ms a frame to decode
  and scale, so it cannot reach 25 fps. The same decoder and scaler as
  1.x; 1.x was not measured on this file.
- **The FPS counter did not appear over a still picture**, a bug from
  part 3: a still is drawn once, so nothing drew the counter, refreshed it
  or took it away. The producer now asks the counter whether it needs a
  frame for its own sake (on switching, and once a second while on). Over a
  still it shows a live 0, which is the truth; 1.x showed 10 because it
  redrew stills ten times a second. Found by the test written for it.
- Verified, device tests on the `trace-test` build (smoke, player, panel,
  timing: 57 cases): all pass. The same player and panel tests passed on
  the `test` build before the counter fix; the `test` build has not been
  flashed since.
- Not verified: the stall reporter's UART output and the trace's overlay
  mark were not looked at.

### 2026-11-19: M2.1 part 3, the animation player

First checkpoint under D42: the smoke set and the changed areas ran on the
device, not the whole suite.

- New component `components/animation_player` (layer `player`), from
  `legacy/main/animation_player_loader.c`, `animation_player_render.c` and
  `legacy/components/loader_service` (moved with `git mv`, then rewritten
  around one idea: the player plays what it is told and reports what
  became of the request).
- A request is reported `ready` (loaded, first frame decoded), then `shown`
  (drawn), or `failed`, each once, by token. The previous artwork plays on
  until the new one is shown, and through any failure. A newer request
  replaces one still loading. These are the `swap_ready` and `swap_shown`
  moments of the design; the pipeline that acts on them is M2.4.
- **The first frame is decoded on the player's task**, not on the display
  pipeline's producer as in 1.x. The producer only adopts a finished clip
  (a pointer swap under a lock held for microseconds). This removes the
  three flags and the semaphore 1.x used to keep the loader from freeing a
  buffer the render task was decoding into, and it takes a decode of up to
  hundreds of milliseconds off the frame-producing path. Its effect on
  timing is unmeasured until part 4.
- **The previous artwork is freed as soon as the new one is shown** (1.x
  kept it until the next load began). Two large stills no longer have to
  fit in memory with a third.
- **A still is drawn once.** 1.x scaled the same picture to the panel ten
  times a second for as long as it was up. It is drawn again only when the
  rotation or the background colour changes.
- **The format is read from the file's first bytes**, not from its name. A
  download saved under the wrong extension plays.
- **Errors say what a caller can do**: `MISSING`, `NOT_PLAYABLE` (not a
  picture, or over the size or byte limit), `CORRUPT`, no memory, the card
  could not be read. The decoders now answer `NOT_PLAYABLE` for a picture
  over 4096 pixels a side instead of the same code as a damaged header; a
  test caught the GIF decoder calling an oversized (possibly sound) file
  damaged, which in M2.4 would have had it deleted.
- **End of animation is the same for every format**: it starts over,
  whatever loop count the file declares (Fab's standing decision). GIF does
  it inside its decoder, the others are reset by the player; `loops` counts
  it the same way for all.
- Jitter fixes carried: the file is read with POSIX `read()` into a buffer
  aligned in address and size (fixes 1 and 6), unchanged.
- The fit and lookup-table code is pure (`player_fit.c`) and host-tested;
  the test hook that carried its own copy (`p3a_test_render.c`) is gone and
  its tests now go through the player with real files.
- `p3a_limits.h` exists now, with its first constant (16 MiB per artwork).
- Verified, host tests (14 programs): fitting, table bounds, format
  sniffing with short input.
- Verified, device tests (smoke, player, decoders: 121 cases, 3 min 42 s):
  the part 2 pipeline tests through the player; a file of each format
  reaches the panel, stills compared exactly with an independent decode;
  GIF, WebP and APNG animations keep their declared pace within 15% over
  four seconds and loop; a transparent still is recomposed when the
  background changes; four kinds of unplayable file each report the right
  error and leave the previous artwork on the panel; a newer request
  replaces an older one; stop; 25 plays, 5 failures and 5 stops leave
  no memory behind.
- Not verified: timing (part 4). A decode failure in the middle of an
  animation (the latch that holds the last good frame) has no test: the
  corpus has no file that fails after its first frame through the player.
  A read error from the card is not provoked.
- Left in the 1.x files for M2.4, not lost: silent retry with its
  blocklist, the on-screen load error, deleting a corrupt cached file and
  dropping it from the index, the LRU touch of a played file, ignoring a
  swap to the file already playing, and the calls that told the scheduler,
  the view tracker and the current-post record about a swap. All of them
  hang off the three reports above.

### 2026-11-19: testing made faster (D42)

- Fab asked for less time spent testing. Decided with him: what runs at a
  checkpoint, what waits for the end of the milestone, how deep tests go,
  and that CI is no longer waited for. The rule is in `TESTING.md` under
  "What runs when", and in `AGENTS.md`.
- Measured first (`--durations`): the 221 device tests take 16 min 38 s.
  Twelve tests take 20 s or more each and about 7 minutes together; one
  (the two-minute wait for an absent network) takes 2 min 25 s alone. They
  are marked `slow`; `-m "not slow"` runs the other 209.
- The screenshot hook answers a checksum (`?crc=1`); the render tests
  download a picture only when the checksum differs.
- Provisioning was looked at and left alone: when the unit is already named
  and online it costs one status query. The time goes to tests that restart
  the unit or forget the network, which is what they test.
- Verified: the checkpoint set for this change (smoke, render, HTTP hooks,
  board display: 43 tests) passes in 3 min 40 s, the flash included. The
  full suite passed before the markers and the checksum went in and has not
  been rerun after; it runs at the end of M2.1.

### 2026-11-18: M2.1 part 2, the display pipeline

- New component `components/display_renderer` (layer `player`). Moved from
  `legacy/main` with `git mv`: `display_renderer.c`, its private header,
  `display_upscaler.c`, `display_ppa_upscaler.c`, `display_fps_overlay.c`
  and the two public headers.
- **The timing code moved unchanged**: the vsync callback, the buffer
  states and the free-buffer wait, the ready queue, and the whole consumer
  (the playhead, the 250 ms resync, the Phase 6 re-baseline, the half-period
  vsync lock, the stale-signal guard), with every frame-trace call. Jitter
  fix 3 (band-stealing upscale) is in `display_upscaler.c` as it was. The
  only edits inside that region are two names (the panel size constants,
  and where "ignore frame delays" is read from).
- What changed is the edges, which in 1.x reached into the UI, the config
  store and the pause state:
  - **Sources.** Three slots (artwork, UI, test patterns), one active. A
    source draws whole frames and may answer "unchanged" (the buffer goes
    back and the producer idles, which 1.x did only for the UI) or fail
    (frame skipped). It is told when its previous frame is not what the
    panel shows, so that "unchanged" is never answered to a blank panel.
    The `ugfx_ui_*` calls are gone; the UI registers itself in Phase 3.
  - **Overlays** register themselves and say whether they also draw over
    the UI. The FPS overlay moved with the renderer. The processing,
    reaction and pin overlays stay in `legacy/` until their features move.
  - **Black output** is `display_renderer_set_blank()` (pause will use it)
    or brightness zero read from the board.
  - **Settings are followed through the change event**: rotation, "ignore
    frame delays", the FPS switch, the background colour. 1.x read the
    config store in the consumer for every frame.
  - **Tasks** come from `p3a_task_create()`: the consumer one priority step
    above the producer, both on the render core; one upscale worker per
    core. `deinit` is gone (tasks are long-lived).
  - **Audit M10**: the wait for the two upscale workers has an overall
    deadline (2 s). It had none, so a worker that never answered hung the
    producer for good.
  - Code for build options this board never uses is removed: RGB565
    panels (15 blocks in the upscaler), a profiling flag, a single-worker
    debug switch. The PPA option is always on, as 1.x shipped it.
- The pipeline starts at boot and owns the framebuffers from then on. With
  nothing registered the panel is black.
- **The panel test commands now go through the pipeline.** `lcd fill`,
  `bars` and `gradient` used to write a framebuffer and present it
  themselves, which cannot coexist with the running pipeline; they are now
  its test source, and screenshots read the buffer the pipeline reports as
  displayed. So the old board tests exercise the real path.
- Test hook `POST /api/v2/test/render`: raw pixels in, fitted to the
  panel through either upscaler in the current rotation.
- Verified, device tests (19 new, 221 in all): a 90 by 90 picture scaled to
  the full panel with all 518,400 pixels compared; seven other shapes
  including 1 by 1, non-integer ratios and 720 by 720; a picture larger
  than the panel; all four rotations following the setting with exactly
  one redraw per change; borders following the background colour; an
  unchanged picture not being drawn again; blanking and brightness zero
  showing black and the picture returning; switching sources and back;
  the hardware (bilinear) scaler within a mean of 6 levels of a bilinear
  reference, smooth, correctly placed and bordered. The nearest-neighbour
  comparisons are exact.
- Verified, builds: release, test, and trace (the frame trace compiled in),
  with the configuration guard.
- Not verified: **timing.** No frame-trace run has been made on 2.0, so the
  jitter pass bar (no presented-frame lateness of 100 ms or more over a
  multi-hour soak) is not re-established; that needs the player (part 3)
  and then a soak (M6.1 at the latest; a short baseline in part 3). The
  upscale deadline path (M10) and the FPS overlay's drawing are not
  exercised: the first needs a stuck worker, the second a running
  animation.
- Numbers: the test image is 1.41 MB, the release image 908 KB.
- One test was loosened, for a reason outside the firmware: the check that
  the workstation's Wi-Fi scan lists `p3a-setup` failed twice in a row.
  Watching both sides showed the network on the air and Windows listing it
  about 30 s later, on its own scan schedule; the test's window was 45 s
  from a point where part of that was already spent. It now waits 150 s.

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
  Fab decided on 2026-11-19 to port them to 1.2.5 (D47); see "Ports to main".
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
