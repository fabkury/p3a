# 2.0 progress ledger

Part of the [2.0 work stream](README.md). This is the first page to read in
any session and the last to update in any checkpoint. Keep "Now" true at all
times.

## Now

| | |
|---|---|
| Phase | Foundations laid. Building has not started |
| Current milestone | None. M0.1 is first |
| Next action | Wait for Fab's go. Then start M0.1 (tree reset and empty app) in [PLAN.md](PLAN.md) |
| Blocked on | Fab's go to start building |
| In flight | Nothing |
| Uncommitted work | None |

Before M0.4 can provision the unit unattended, Fab needs to put the Wi-Fi
password into `%USERPROFILE%\.p3a-lab\secrets.json` (the `FILL-ME-IN` value).
Nothing before M0.4 needs it.

## Dev unit

| | |
|---|---|
| Running | 1.2.4 release build of 2026-10-02, untouched by this work stream so far |
| Last flashed by this stream | Never |
| SD card | 32 GB, healthy (D8). Still holds 1.x content under `/p3a2`; to be wiped at M0.4 or M1.3 |
| 1.x flash backup | `%USERPROFILE%\.p3a-lab\backups\p3a-fab_v1.2.4_full-flash_2026-11-17.bin` (see the session log for size and hash) |
| Secrets file | Created 2026-11-17 with the four API keys read from the unit; Wi-Fi password still to be filled in by Fab |

## Milestones

Status is one of: not started, in progress, done (date, commit, tiers),
reopened (why).

| Milestone | Status |
|---|---|
| M0.1 Tree reset and empty app | not started |
| M0.2 Host test harness | not started |
| M0.3 Continuous integration | not started |
| M0.4 Device lab | not started |
| M1.1 util | not started |
| M1.2 board | not started |
| M1.3 storage | not started |
| M1.4 settings | not started |
| M1.5 net base | not started |
| M2.1 Display pipeline and decoders | not started |
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

- None yet.

Cleared:

- None yet.

## Open questions for Fab

Decisions that are still open, with the milestone that needs them. Details
and proposals are in [DECISIONS.md](DECISIONS.md) under "Open decisions".

| Id | Question | Needed before |
|---|---|---|
| Q1 to Q10 | Ten 1.x behaviours that look accidental (brightness not restored at boot, API keys readable over the LAN, no update check for 12 hours, and others). Keep or change each? Listed in [PARITY.md](PARITY.md) under "Behaviours to decide" | M1.3 at the earliest; each row names its milestone |
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

The remaining open decisions (O7 to O11) may be taken as working decisions
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

- Nothing yet.

## Session log

Newest first. One entry per session or checkpoint: date, what was done,
what was verified and at which tier, the commit.

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
