# p3a 2.0 work stream

p3a 2.0 rebuilds the firmware and the web UI on a new architecture. It is a
clean break from 1.x: a 2.0 flash is a fresh device, nothing on the SD card
or in NVS carries over, and there is no OTA path from 1.x (2.0 is installed
over USB). The work happens on the `v2` branch. `main` stays the 1.x
production line.

The build will take weeks and many sessions. This folder is its memory:
everything needed to stop at any reasonable point and resume later is written
here, and keeping it current is part of the work, not something done after.

## Where things are

| Page | Holds | Read it |
|---|---|---|
| [PROGRESS.md](PROGRESS.md) | The ledger: where the work stands, the next action, the manual checklist, open questions, the session log | First, every session |
| [PLAN.md](PLAN.md) | Phases and milestones with scope, tests and exit criteria; a table tracing every review finding to a milestone | Before starting a milestone |
| [DECISIONS.md](DECISIONS.md) | Every decision with its date and reason, the 1.x decisions that stay in force, and the decisions still open | Before touching a decided area, and whenever a choice comes up |
| [TESTING.md](TESTING.md) | The test tiers, the tooling, the dev unit rig, and what "verified" means here | Before writing or running tests |
| [PARITY.md](PARITY.md) | Every 1.x capability and where it lands in 2.0 | When moving a feature, and before release |
| [../2.0-architecture-review/](../2.0-architecture-review/README.md) | The design: why 1.x is being rebuilt, the target skeleton, the ideas list | For the reasoning behind a milestone |

The review folder is the design and does not track progress. This folder
tracks progress and does not repeat the design.

## Starting a session

1. `git status` and `git log -5` on `v2`. Uncommitted changes mean the last
   session stopped mid-step or a signing prompt went unanswered; the "Now"
   section of the ledger says which.
2. Read "Now" in [PROGRESS.md](PROGRESS.md): current milestone, next action,
   anything in flight, anything blocked.
3. Read that milestone's entry in [PLAN.md](PLAN.md).
4. Check the dev unit against "Dev unit" in the ledger (what it was last
   flashed with). Reflash if in doubt; it costs a minute.
5. Carry on from the next action.

## Checkpoints

A checkpoint closes one coherent step: a module moved, a test tier added, a
milestone finished. There is at least one per milestone and usually several.

1. The step's tests pass at the tiers its milestone names.
2. [PROGRESS.md](PROGRESS.md) is updated: "Now", the milestone table, a line
   in the session log, any new manual checks, any new open questions.
3. Every decision made along the way is in [DECISIONS.md](DECISIONS.md).
4. [PARITY.md](PARITY.md) rows are updated for capabilities that landed.
5. One signed commit holds the work and its ledger update together, and `v2`
   is pushed.

If the signing prompt goes unanswered, the work stays uncommitted in the
tree, "Now" says so under "Uncommitted work", and the session carries on.
The next checkpoint commits both.

When a session has to stop in the middle of a step, write an "In flight"
note under "Now" before stopping: what is half done, what state the tree is
in, what the dev unit is running.

## Rules of the stream

1. **The scope is the review.** All 25 verdicts, changes A to D, the three
   ideas, and full 1.x parity minus PICO-8. A new idea becomes an entry in
   `../2.0-architecture-review/ideas.md` and waits for Fab to triage it.
   Scope never grows silently.
2. **Arrows point down.** A component depends only on layers below it, and
   every dependency is a `REQUIRES`. No `WHOLE_ARCHIVE`, no
   `__attribute__((weak))`, no component includes a header from `main/`. CI
   enforces this from M0.3.
3. **Code enters the new tree through a milestone.** It is either moved from
   `legacy/` (stripped of upward includes, with that file's known issues
   fixed or re-recorded) or written new, and it lands with tests at the tier
   its milestone names. Nothing in the new tree includes or links anything in
   `legacy/`.
4. **Always bootable.** From M0.1 on, every commit pushed to `v2` builds, and
   every milestone ends with the dev unit booting the result.
5. **Release builds carry no test code.** Test hooks live behind
   `CONFIG_P3A_TEST_HOOKS`, set only by the test overlay, and compile to
   nothing otherwise.
6. **Secrets stay out.** Wi-Fi credentials, API keys and flash backups live
   in `%USERPROFILE%\.p3a-lab\`, outside the repo and outside Dropbox. They
   never appear in the repo, in these documents, in logs kept as evidence, in
   commit messages or in chat.
7. **`main` is not touched by 2.0 work.** A 1.x fix goes to `main` and is
   added to "Ports from main" in the ledger so that 2.0 picks it up by hand.
8. **Say which tier verified it.** "Verified" always names the tier (host
   tests, CI build, device tests, soak, manual). Anything only Fab can check
   goes on the manual checklist and is reported as awaiting it.
9. **Inherited decisions hold.** The 1.x product decisions listed in
   [DECISIONS.md](DECISIONS.md) stay in force unless Fab reopens one.
10. **AGENTS.md on `v2` describes the tree as it is.** It is read at the
    start of every session, so it changes in the same commit as the tree.
    The user-facing documents (`README.md`, `docs/HOW-TO-USE.md` and the
    rest) keep describing 1.x until M5.4 rewrites them in one pass.

## Environment

| Item | Value |
|---|---|
| Dev unit | Waveshare ESP32-P4-WIFI6-Touch-LCD-4B, chip rev v1.0, attached by USB. Two MACs identify it: `30:ED:A0:E2:09:37` is the ESP32-P4 base MAC that esptool prints, `CC:BA:97:F0:2C:88` is the Wi-Fi MAC that 1.x reports in `/status`. Owned by this work stream: flash, NVS and SD card may be erased |
| UART | COM5 (CH343 bridge, 115200). It has enumerated as COM9 and COM11 on other days; list ports with `Get-CimInstance Win32_PnPEntity` rather than opening them. Opening the port with .NET `SerialPort` or plain `idf.py monitor` resets the board; `host/jitter-lab/serial_logger.py` does not |
| Network name | `p3a-fab.local` when the device name is `fab`. A freshly erased unit comes up as plain `p3a.local`, which on this LAN is a different, second device; the lab sets the device name during provisioning and otherwise addresses the unit by the IP it prints on UART |
| SD card | 32 GB (reports itself as "SDABC", 29,820 MB), FAT32, confirmed healthy by Fab on 2026-11-17. SD errors are firmware bugs until proven otherwise |
| Secrets | `%USERPROFILE%\.p3a-lab\secrets.json`: Wi-Fi SSID and password, the Giphy, Klipy, Harvard and Smithsonian keys |
| 1.x flash backup | `%USERPROFILE%\.p3a-lab\backups\`: the unit's full 32 MB flash as it ran 1.2.4, taken before the first erase. It holds the unit's Makapix identity and Wi-Fi credentials |
| Firmware toolchain | ESP-IDF v5.5.4 through EIM at `C:\Espressif`. Activation and the `ESP_IDF_VERSION=5.5` and silicon-revision traps are in `AGENTS.md` |
| Host toolchain | MinGW-w64 gcc 16.1, CMake, Ninja and CTest on PATH (WinLibs); Python 3.14 with pytest, pyserial, requests and Pillow; Node.js; WSL Ubuntu 24.04 for Linux-only checks |
| USB-Serial-JTAG | Not wired to the laptop. No OpenOCD; diagnosis is by UART log, core dump and in-firmware tracing |
