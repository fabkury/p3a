# 2.0 ideas list

Part of the [2.0 architecture review](README.md). A running list of what
2.0 should contain, kept here so the ideas and the proposed skeleton in
[target-architecture.md](target-architecture.md) stay next to each other.
Each entry says what the feature is, where it lands in the skeleton, and
what it depends on. New ideas are appended with the date they were added;
nothing here is scheduled or designed in detail yet.

## Founding changes (2026-11-06)

| # | Idea | Lands in | Notes |
|---|---|---|---|
| A | Single binary: web UI embedded, no LittleFS, no web UI OTA | `net` | See the web UI page. About 150 KB gzipped after shrinking the PWA icons. |
| B | Cache records grow from 64 to 192 bytes | `library`, `record_t` | See the storage page. Cheaper than today once the per-node hash allocations go. |
| C | Suppress repetitions (RAM-only, reset by reboot) | `player`, picker | Needs the `swap_shown` event and one pick function. |
| D | Filter aspect ratio | `player`, picker | Design in `docs/deferred/aspect-ratio-filter.md`; `width`/`height` in the record header. |

## Added 2026-11-17

### 1. Clock overlay (default off)

Draws the current time on top of whatever artwork is playing. Settings:
font style and size from a few predefined built-in choices, screen position
(top-left, top-center, top-right, and so on), text color, border color,
border opacity (alpha 1 to 255), border thickness (1 to 6 px).

**Lands in** `ui`, as one more overlay baked by the display producer beside
the FPS, processing, reaction and pin overlays (`main/display_renderer.c`
already does this per frame; in 2.0 the overlays live in `ui` and read the
screen model).

**Depends on.** Idea 3 for the fonts. SNTP sync for the time; the overlay
should hide, not show a wrong time, until the clock is trusted. The settings
schema (storage page, section 4) so the eight settings are one schema entry
each and the UI renders them without hand-written fields. Rotation: the
overlay must follow the configured screen rotation like the other overlays
do.

**Open questions.** 12 or 24 hour format and whether to show seconds (a
seconds digit forces a redraw every second even on a static artwork, which
the static-frame cache in `animation_player_render.c` currently avoids).
Whether the clock should also appear during channel messages and the info
screen.

### 2. Giphy and Klipy branding (default on)

When a Giphy or Klipy artwork is letterboxed and the band below it is at
least some minimum height, the matching logo is drawn in that band. Nothing
is drawn on artworks that fill the screen or whose band is too small. The two
logos ship as embedded image files.

**Lands in** `ui` as an overlay, with the letterbox geometry coming from the
player. The producer already knows `upscale_offset_y` and
`upscale_scaled_h` per buffer (`main/animation_player_priv.h`), so "band
height below the artwork" is `dst_h - offset_y - scaled_h` with no new
measurement. Source attribution comes from `record_t.source`, so no
path-string matching.

**Depends on.** `EMBED_FILES` for the two logos (the firmware page, item
24, already proposes replacing the hex arrays). The attribution rules of the
Giphy and Klipy partner terms decide the minimum logo size and clear space,
which in turn fix the minimum band height; check both before choosing
numbers. Rotation applies here too: "below" means below in the displayed
orientation.

**Open questions.** Whether the logo should also be drawn in a side
pillarbox when a tall artwork leaves no bottom band, or only ever at the
bottom as stated. Whether the Makapix and museum channels want a matching
discreet attribution; not asked for, noted only so the overlay is designed
with a source-to-badge table rather than two special cases.

### 3. Font refresh: VEXED pixel fonts

All on-screen text (not the web UI) moves from DejaVu Sans to one or more
pixel fonts from the VEXED collection at
`C:\Users\fab\Dropbox\PC\F\Estudo\Tecnologia\assets\pixel-fonts\vexed`
(62 families, each with TTF variants and PNG sheets). Pixel fonts are drawn
at native size or integer multiples only. The chosen fonts are embedded; no
download.

**Lands in** `ui`. This is the right moment because `main/ugfx_ui.c:36-46`
records that every `gdispOpenFont` call today silently falls back to
DejaVuSans32 through a glob-matching bug, and the whole UI was laid out
against that fallback. A font change therefore means re-verifying every
screen on the device anyway, so 2.0 should pick the fonts first and lay the
screens out once against them.

**Depends on.** A decision on the uGFX text path versus a small in-house
bitmap renderer. uGFX's font converter produces anti-aliased glyphs from
TTFs, which is the opposite of what a pixel font wants; the PNG sheets in
each VEXED family are the better source, and a glyph-sheet renderer with
integer scaling is a few hundred lines. If uGFX is kept only for text, this
is also the point to ask whether it is still worth its 54k lines of source.

**Open questions.** Which family or families, and at which base sizes, for
the three roles the UI has today (large status text, body text, small
labels). Whether the clock overlay (idea 1) shares the UI fonts or gets its
own set of two or three. License terms of the chosen families for embedding
in an Apache 2.0 firmware; the per-family `*-Info.md` files should state
this.

## Added 2026-11-18

### 4. A list of nearby networks on the setup page

**What it is.** The setup page asks the user to type the Wi-Fi name, as 1.x
did. A scan would let them pick it from a list, with signal strength, and
would remove the commonest setup error after a mistyped password: a
mistyped name.

**Where it lands.** `components/p3a_net`: a `GET /api/v2/wifi/scan`
answered as a job (a scan takes two to three seconds), and a list on
`static/setup/index.html` with the typed field kept as the fallback for
hidden networks.

**What it depends on.** The radio must be in access-point-plus-station mode
to scan while serving setup. That mode has not been tried through the
co-processor, including the units whose co-processor is stuck on firmware
2.7.0.

**Open questions.** Whether scanning disturbs the phones joined to the
setup network enough to matter (the radio leaves its channel briefly).
Whether to scan once when setup starts and cache the list, or on request.

## How to add an idea

Append a dated section with the same four parts: what it is, where it lands
in the skeleton, what it depends on, open questions. If an idea changes the
skeleton itself, update `target-architecture.md` in the same commit.

## Added 2026-11-23

### 5. On-screen text beyond Latin

**What it is.** The device's own screens draw ASCII and the Latin-1
accented letters; anything else appears as "?". Titles and artist names
from Makapix and the museums can be in any script. Fab raised it when
uGFX was dropped (D60) and has not decided whether it is wanted.

**Where it lands.** `tools/gen_font.py` (the `CODEPOINTS` list) and
`components/p3a_ui/fonts/`. The file format and the renderer need no
change: a font file holds any character up to U+FFFF, found by binary
search.

**What it depends on.** The font. Everyday Ample has Latin-1 and nothing
beyond it (checked in the TTF: 164 characters, none Cyrillic or Greek), so
every other script needs a second pixel font with those glyphs and a way to
fall back from one font to the other. Everyday Standard, of the same
series, has Cyrillic and the extra letters of Polish and Turkish and would
be the first candidate. Arabic and Hebrew also need right-to-left layout,
which the renderer does not do.

