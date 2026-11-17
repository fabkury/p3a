# 2.0 testing

Part of the [2.0 work stream](README.md). 1.x was tested by hand. 2.0 is
tested by machines wherever a machine can do it, and by the agent on the dev
unit wherever that works; Fab's hands and eyes are kept for what nothing else
can check.

This page is the strategy and the conventions. The tooling it describes is
built in Phase 0 of [PLAN.md](PLAN.md); until a tool exists, its section
here is the specification for it.

## Tiers

| Tier | What | Runs on | Run by | When |
|---|---|---|---|---|
| T1 | Host unit tests | Workstation and CI | Agent, CI | Every change to logic |
| T2 | Build checks and lints | CI, and locally before a push | Agent, CI | Every push |
| T3 | Device tests | The dev unit | Agent | Every milestone, and after any change to code that runs on hardware |
| T4 | Web UI tests | Node, and a browser against the dev unit | Agent | Phase 5, and after any web UI change |
| T5 | Soaks | The dev unit, for hours | Agent | Phase 6, and after changes to the display pipeline, memory or SD I/O |
| T6 | Manual checks | The dev unit, a phone, a second board | Fab | Batched (D10) |

"Verified" always names its tier. A change is reported as, for example,
"host tests pass, device tests pass, awaiting manual check M1.2-a", never as
plainly done.

### T1 Host unit tests

What belongs here: everything that is logic and not hardware. The record
and index codecs, the framed container, the slot table and bitmap, the blob
reference counts, SWRR and the pick function, the repetition ring, the
aspect predicate, the settings schema and its validation, playset JSON and
packing, the refresh state machine, the loader pipeline against fakes, the
route matcher, the response envelope, the error mapping, each source's
listing parser fed with recorded responses.

- Location: `tests/host/`, one test file per unit, fixtures in
  `tests/host/fixtures/`.
- Tooling: Unity, CMake, CTest. A FreeRTOS stub covers what the logic layers
  touch; it grows only when a test needs it.
- Compilers: MinGW gcc on the workstation, gcc on CI. CI also runs the suite
  under AddressSanitizer and UndefinedBehaviorSanitizer; run it in WSL
  locally when chasing a memory bug.
- Rule: logic gets its host test before it first runs on the device. A bug
  found on the device in host-testable code gets a host test that fails
  first.
- Recorded responses contain no API keys; strip them when recording.

### T2 Build checks and lints

- The firmware builds in both configurations (release and test).
- The image is at most 90 percent of its app slot.
- The layer rule: each component's `REQUIRES` is checked against the layer
  table.
- Forbidden in project code: `__attribute__((weak))`, `WHOLE_ARCHIVE`,
  includes from `main/`, anything from `legacy/`.
- The generated `sdkconfig` shows `CONFIG_ESP32P4_REV_MIN_FULL=1`.
- A release build contains no test-hook symbols.

### T3 Device tests

pytest drives the dev unit: it flashes a test build, provisions it, calls
the HTTP API and the test hooks, and asserts on responses, screenshots and
the serial log.

- Location: `tests/device/`. Lab tooling: `host/p3a-lab/`.
- Every test captures the serial log. A panic, an assert, a watchdog report
  or a boot loop fails the test whatever else it asserted.
- The fixtures refuse to talk to any device that is not the dev unit
  (checked by MAC address), because another p3a lives on the same LAN.
- Tests that reach the internet (Giphy, Klipy, museums, Makapix, GitHub) are
  marked `live` and can be deselected. A `live` failure is checked against
  the service before it is blamed on the firmware.
- Tests leave the unit as they found it, or say what they changed.
- Screenshot tests compare against reference images in
  `tests/device/references/` with a small tolerance. A reference is added or
  replaced only after the agent has looked at the image, and replaced
  references are listed in the checkpoint's ledger line.

### T4 Web UI tests

- `node --test` for the shared JavaScript (`api.js`, the museum browse
  adapters, the settings renderer), with no npm dependencies.
- Page checks in a real browser against the dev unit: each page loads with
  no console errors, each control does what it says.

### T5 Soaks

- The jitter pass bar, unchanged from `docs/jitter/README.md`: on a build
  whose only difference from release is the frame trace, no in-scope
  presented-frame lateness of 100 ms or more over 3 hours under the work
  mix. The `host/jitter-lab` tools are kept for this and adapted to
  `/api/v2`.
- The memory soak: a full refresh cycle across many channels with downloads
  running, watching the DMA-pool watermark. No assert, no SD livelock.
- A multi-day run before release.

### T6 Manual checks

Things only a person can do: how the panel really looks, real fingers on
the glass, pulling power, swapping the card, the captive portal from a
phone, USB mass storage from a computer, the registration code on the
Makapix site, a real update from GitHub, the web flasher on a second board.

Each check is one line on the manual checklist in
[PROGRESS.md](PROGRESS.md), written so that Fab can do it cold: what to do,
what should happen. It carries the id of the milestone that created it. A
failed check reopens that milestone.

## The dev unit rig

### Builds

| Configuration | sdkconfig | Use |
|---|---|---|
| Release | `sdkconfig.defaults` | What ships. No test or trace code |
| Test | release plus `sdkconfig.test.defaults` | Device tests. Adds `CONFIG_P3A_TEST_HOOKS` |
| Trace | release plus `sdkconfig.trace.defaults` | The jitter pass bar |
| Diag | release plus `sdkconfig.diag.defaults` | Investigating a regression |

Overlay builds go to their own build directories with their own generated
`sdkconfig`, as in the jitter work. The dev unit normally runs the
test build.

### Test hooks

Present only when `CONFIG_P3A_TEST_HOOKS` is set. Added as milestones need
them.

| Hook | Transport | Purpose | From |
|---|---|---|---|
| Console: `version`, `reboot` | UART | Identify the unit and its firmware; restart it | M0.4 |
| Console: `task`, `errname`, `job`, `jobinfo`, `event`, `mem` | UART | Exercise the util layer: task policy, error names, job runners, event fan-out and overflow, memory pressure (including taking and returning DMA-pool memory) | M1.1 |
| Console: provision | UART | Set Wi-Fi credentials, device name and API keys on a fresh unit | M1.4, M1.5 |
| Console: `lcd`, `button` | UART | Paint and present test patterns, read pixels and a CRC back, save a downscaled screenshot (`lab.py shot`), set the backlight, count BOOT presses. Touch and card probes follow in M1.2 | M1.2 |
| Screenshot | HTTP | The front buffer as an image. This is how the agent sees the screen | M1.5 |
| Clock | HTTP | Set the wall clock, for eviction, the clock overlay and certificate logic | M1.5 |
| File put and play by path | HTTP | Put a test asset on the card and play it without a playset | M2.1 |
| Touch injection | HTTP | Synthetic taps, swipes, rotations, long presses | M3.4 |
| Fault injection | HTTP | Trip the SD health latch, simulate low DMA memory, fail a fetch | When a test needs one |
| Frame trace | HTTP | The existing `/api/debug/frames` family | M2.1 |

### Provisioning

A freshly erased unit has no Wi-Fi credentials, and the captive portal
needs a person. The lab instead sends the credentials from
`%USERPROFILE%\.p3a-lab\secrets.json` to the test console over UART (D20).
It also sets the device name to `fab`, so the unit answers at
`p3a-fab.local` and not at `p3a.local`, which is another device on this LAN.
The lab reads the unit's IP address from the serial log and prefers it to
mDNS.

The secrets file is never read into a log, a test report or a commit. The
captive portal itself is a manual check.

### Serial port

The UART resets the board when opened by .NET `SerialPort` or plain
`idf.py monitor`. The lab opens it the way
`host/jitter-lab/serial_logger.py` does, which does not. Flashing needs the
port free, so the lab stops its own capture first. The USB cable is fragile:
a reset with a lost ROM banner and an "Access is denied" from the logger is
the cable, not the firmware.

### Returning to 1.x

The unit's 1.2.4 state is saved as a full flash image (see the ledger). To
put it back, write that image at offset 0 with esptool. This is for
emergencies; normal 1.x work does not need the unit to keep its old state.

## What stays manual, and why

| Area | Why a machine cannot do it |
|---|---|
| Panel appearance: colour, backlight, smoothness as seen | The screenshot hook reads the frame buffer, not the glass |
| Real touch | Injection bypasses the controller and the finger |
| Power cuts | Nothing on the rig can cut power |
| Card swaps, bad cards, small cards | Physical |
| Captive portal on a phone | Needs a second radio and a person |
| USB mass storage | Needs the second USB port cabled to a computer |
| Makapix registration and site-side actions | Needs Fab's account |
| A real GitHub release update | Fab cuts releases |
| The web flasher on a fresh board | Needs a second board |
