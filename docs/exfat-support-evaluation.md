# exFAT Support Evaluation (mount-only)

**Date:** 2026-07-27
**Status:** Evaluation only — no decision, nothing implemented.
**Scope:** Mount-only. p3a would accept factory-formatted exFAT cards read/write. The
on-device formatter keeps producing FAT32; SD-health flows unchanged; no exFAT
formatting offered anywhere.

---

## Why this comes up

The SD specification mandates that SDXC cards (anything above 32 GB) ship
factory-formatted as exFAT. Today p3a cannot mount those: the user gets the
**"No Usable SD Card"** screen, and the only on-device escape is a **destructive**
FAT32 format. The computer-side alternative is also unfriendly — Windows' built-in
Format dialog caps FAT32 at 32 GB, so users need guiformat or Rufus. All of this is
documented as a caveat block in `docs/HOW-TO-USE.md:48`.

The trend is one-directional: cards ≤32 GB are getting harder to buy, so over time a
larger share of first-run users will hit this wall with the card they already own —
possibly one carrying data they care about.

## Current state (verified in repo and IDF source)

- **Nothing in p3a itself is FAT32-specific.** The mount is
  `bsp_sdcard_mount()` in the managed Waveshare BSP →
  `esp_vfs_fat_sdmmc_mount()`; filesystem detection is FatFs's `f_mount`.
  Free-space queries go through `esp_vfs_fat_info()`
  (`components/storage_eviction/storage_eviction.c:165`), `fs_atomic` is
  rename-based, the sd_health latch is I/O-error driven, and the filename
  sanitizers (`sd_path.h:244`, `pin_lists.c:796`) already speak of "FAT/exFAT".
  The exFAT blocker is entirely in the FatFs build configuration.
- **ESP-IDF hardcodes `FF_FS_EXFAT 0`** in `components/fatfs/src/ffconf.h` (FatFs
  R0.15, `FFCONF_DEF 80286` in IDF v5.5.2) and offers **no Kconfig switch**. This is
  deliberate: in [issue #6601](https://github.com/espressif/esp-idf/issues/6601)
  (Feb 2021), igrr (Espressif) explained that Microsoft's exFAT patent pledge via the
  Open Invention Network covers **Linux implementations only** — it would not cover
  FatFs's independent exFAT implementation on an ESP32 — so Espressif won't enable it.
  The request was closed; there is no sign of that stance changing.
- **The sd_format probe** (`main/sd_format.c:412-416`) treats "card answers but
  `f_mount` fails" as the needs-format case; exFAT cards land there today. With exFAT
  mountable, they would take the "card OK" path with **zero logic changes** — only the
  comment listing exFAT among foreign filesystems goes stale. NTFS/blank/corrupt cards
  still reach the format offer as before.
- **User-facing FAT32 references** that would need updating: `docs/HOW-TO-USE.md:36`
  and the `:48` caveat block, the warning screen text "and format it as FAT32 for p3a"
  (`main/ugfx_ui.c:949` — still accurate, but its context changes), and the
  `sd_format.h`/`.c` doc headers. The webui SD-health banner (`webui/index.html:827`)
  is format-agnostic and needs no change.

## What implementation would take

1. **Shadow the IDF `fatfs` component**: copy `esp-idf/components/fatfs/` into repo
   `components/fatfs/` (project components override same-named IDF components). This
   is the same pattern as the existing `components/espressif__libpng` fork.
2. **`ffconf.h`: `FF_FS_EXFAT 1`.** Prerequisites are already met: LFN is enabled
   (`CONFIG_FATFS_LFN_HEAP`) and the toolchain is C99+. No other config changes —
   `FF_MAX_SS 4096` coexists fine with exFAT on 512-byte-sector cards, and `FF_FS_LBA64`
   can stay 0 (SDXC tops out at 2 TB; SDUC is out of scope).
3. **Pin the formatter to FAT32.** This is the non-obvious trap: IDF's
   `vfs_fat_sdmmc.c` passes `FM_ANY` to `f_mkfs` at both format sites (lines 224 and
   518 in v5.5.2), and FatFs auto-selects exFAT for any volume ≥ 0x4000000 sectors =
   32 GiB once `FF_FS_EXFAT` is compiled in (`ff.c:6050`). Left alone, the on-device
   formatter would silently start producing exFAT on >32 GB cards — contradicting the
   on-screen "format it as FAT32 for p3a" promise and the mount-only scope. The fork
   must change `FM_ANY` → `FM_FAT | FM_FAT32` at both sites.
4. **Text/doc updates** as listed above (HOW-TO-USE, sd_format headers, stale probe
   comment).
5. **Device testing**: factory-exFAT card end-to-end (vault sharding, downloads,
   eviction watermarks, playback), formatter-still-FAT32 on a >32 GB card, NTFS card
   still reaches the format offer, power-cut soak while caching, USB-MSC round-trip
   (host reads/writes the exFAT card, device remounts cleanly).

So: a permanent fork carrying a **~3-line functional diff** across 2 files, plus text
updates and a test matrix.

## Pros

- **Store-bought cards just work.** A new user inserts any >32 GB card from the drawer
  and p3a plays immediately — no wipe, no Rufus, no caveat paragraph. This is the
  single biggest remaining SD onboarding friction, and it matters more as outreach
  brings in non-tinkerer users.
- **Non-destructive path for cards carrying data.** Today the on-device fix erases the
  card ("vacation photos" scenario); mount-only exFAT preserves it.
- **Host reformats stop bricking the card.** If a user reformats over USB-MSC and
  Windows/macOS defaults to exFAT (which they do for big cards), the card keeps working
  in p3a instead of triggering the No-Usable-SD flow.
- **Docs shrink**: the HOW-TO-USE `:48` caveat block mostly disappears.
- Minor: free-space scans (used by eviction watermarks) use exFAT's allocation bitmap
  rather than a FAT32 full-FAT walk — marginal in practice since FAT32's FSINFO
  usually short-circuits the scan.
- Irrelevant-but-true: single files >4 GiB become possible (pixel-art payloads never
  approach this).

## Cons

- **Permanent fork of a core IDF component.** Every IDF bump (the 6.0 migration is
  already on the radar — `docs/ESP-IDF-v6.0/migration-report.md`) requires re-copying
  the component and re-applying the patch, with silent-divergence risk if forgotten.
  The libpng fork shows the process is manageable, but it's standing process debt on a
  much more central component.
- **Espressif-unsupported territory.** `FF_FS_EXFAT=1` has zero Espressif CI or test
  coverage — their entire VFS layer contains exactly one exFAT-aware `#if` (a log
  format string). The FatFs core is mature (exFAT since 2016, R0.12), but any
  exFAT-specific bug in the VFS/sdmmc glue is ours alone to find and fix.
- **The fleet splits into two on-card formats.** Every future FS-adjacent bug report
  starts with "FAT32 or exFAT?"; the release test surface for SD features roughly
  doubles.
- **Robustness is likely somewhat worse, not better.** exFAT keeps a single FAT plus
  an allocation bitmap (no redundant copy); FAT32 as formatted by p3a keeps two FATs.
  FatFs journals neither and has no fsck for either, and p3a writes continuously
  (cache downloads + eviction) on a device users unplug at will. The sd_health latch
  catches the fallout identically either way, and recovery (on-device format) converts
  the card to FAT32 — but desktop recovery tooling has more to work with on FAT32.
- **Licensing (noted briefly, per project posture).** FatFs's exFAT is an
  implementation of Microsoft's patented spec (US Pat. App. Pub. 2009/0164440); the
  [FatFs application note](http://elm-chan.org/fsw/ff/doc/appnote.html) states
  commercial products may need a Microsoft license, and the 2019 OIN pledge covers
  Linux-kernel implementations only — which is exactly why Espressif declines to ship
  it. For open-source hobby firmware with giveaway units, practical exposure is
  negligible; revisit before ever selling devices.

## Costs (measured, not estimated)

Measured by compiling IDF v5.5.2's FatFs standalone with `riscv32-esp-elf-gcc` 14.2 at
`-Os` under this project's exact FATFS sdkconfig values, `FF_FS_EXFAT` off vs on:

- **Flash: +7.3 KiB** (`ff.c` .text 16,223 → 23,503 bytes; `ffunicode.c` unchanged).
  Negligible against the app partition.
- **RAM: negligible.** `FIL` +40 B, `FF_DIR` +32 B, `FILINFO` +8 B, `FATFS` +8 B, plus
  one ~608 B heap allocation per mounted volume (exFAT directory scratch buffer,
  `MAXDIRB(255)`, heap-side because LFN is heap-mode).
- **One-time effort:** small — component copy, ~3-line patch, text updates. The real
  one-time cost is the device-test matrix (requires a >32 GB factory-exFAT card and a
  power-cut soak).
- **Ongoing effort:** fork re-sync per IDF bump; doubled FS-format test surface per
  release.

## Risks

1. **Silent formatter behavior change** if the `FM_ANY` pin (implementation step 3) is
   missed or lost in a fork re-sync — >32 GB cards would quietly format as exFAT.
2. **Fork drift** across IDF bumps — mitigated by documenting the diff in the fork's
   README (libpng-fork precedent).
3. **Untested-by-vendor config** — first-mover debugging burden for any VFS-layer
   exFAT bug.
4. **Field corruption reports skew** toward exFAT cards over time (single-FAT,
   heavy-write device, user power cuts); indistinguishable at triage from generic SD
   failure without asking the format.
5. **Patent gray zone** if distribution ever turns commercial (see Cons).

## Bottom line

Technically this is cheap: a ~3-line functional patch inside a shadowed component,
+7.3 KiB flash, near-zero RAM, and p3a's own code needs no changes at all.
Operationally it is not free: a permanent fork of a core IDF component in a
configuration Espressif explicitly refuses to support, plus a doubled on-card format
matrix forever.

The on-device FAT32 formatter (shipped 1.1.1) already unblocks every card —
destructively. Mount-only exFAT buys exactly one thing over it: big store-bought
cards, including ones carrying data the user wants to keep, work instantly with no
wipe. For the current builder-heavy audience that's a nice-to-have; for a
non-tinkerer audience arriving via outreach it's the difference between "it just
worked" and a destructive-format decision on first boot.

**Recommendation:** defer, with a defined trigger — implement when either (a) field
reports show users hitting the No-Usable-SD screen with data-carrying cards, or (b) an
outreach wave is about to put units/instructions in front of non-tinkerers. If
triggered, do it exactly as scoped here (mount-only, formatter pinned to FAT32) and
document the fork diff in `components/fatfs/README.md` following the libpng-fork
precedent.
