# Proposal C — **Pixel Console** (Emissive HUD)

> *The web UI is the instrument panel of a display device. Colors don't sit on the screen — they
> are lit.*

## Concept

p3a is hardware: an ESP32-P4, a co-processor, an IPS panel, USB, touch. **Pixel Console** dresses
the web UI as the *control surface* of that hardware — a hybrid of retro game console and lab
instrument. Deep instrument-black chassis, a pixel-grid substrate, monospace readouts, and a small
set of **emissive** accent colors that glow like lit LEDs / pixels (each accent paired with a soft
bloom). The hue wheel returns as a *live, cycling* motif rather than a static rainbow.

Where *Spectrum* is flat-bright-systematic and *Gallery* is warm-quiet, **Pixel Console** is
dark-emissive-techy with a strong personality: this is the option that looks like nothing else and
reads instantly as "a device you control."

**How it resolves the tension:** color is **sparse and emissive**. On a near-black chassis, a single
glowing cyan control or a green "ON" LED carries enormous weight — you need very little color, and
what you use feels intentional and energetic. Color = signal, never decoration.

## Who it's for

The maker/tinkerer and the retro-future enthusiast — the person who enjoys that p3a is an
ESP32 project, flashes firmware over OTA, and reads a PICO-8 monitor. It celebrates the hardware.

## Palette

### Instrument chassis (dark, the only mode — see "Exhibit mode" for calm)

| Token | Hex | Use |
|-------|-----|-----|
| `--panel-0` | `#07080A` | deepest black — body |
| `--panel-1` | `#0E1014` | chassis surface |
| `--panel-2` | `#161A20` | raised module |
| `--panel-3` | `#20262E` | controls / inputs |
| `--grid`    | `#2A323C` | 1px pixel-grid lines & module borders |
| `--readout-dim` | `#7E8A96` | secondary readout text |
| `--readout` | `#C7D0D9` | primary LCD-readout text (cool gray) |

### Emissive accents (each ships with a matching glow)

| Token | Hex | Glow (box-shadow) | Role |
|-------|-----|-------------------|------|
| `--glow-cyan`    | `#3DF0E0` | `0 0 12px #3DF0E060` | **primary** control (replaces `#667eea`) |
| `--glow-lime`    | `#8CF04A` | `0 0 12px #8CF04A60` | **success / ON / active** |
| `--glow-amber`   | `#FFC23D` | `0 0 12px #FFC23D60` | **warning / cooldown** |
| `--glow-red`     | `#FF5563` | `0 0 12px #FF556360` | **danger / fault** |
| `--glow-magenta` | `#FF5CD2` | `0 0 12px #FF5CD260` | pins / accent 2 |
| `--glow-violet`  | `#9D7BFF` | `0 0 12px #9D7BFF60` | secondary |

### Mapping onto `common.css`

```text
--c-primary      → --glow-cyan #3DF0E0  (+ paired bloom on hover/active)
--c-gradient     → flat --panel-0; the "gradient" becomes a conic hue-cycle used ONLY by the
                   loading spinner and the boot animation
--c-card-bg      → --panel-1 / --panel-2 (opaque; no glass blur — use the grid + 1px --grid border)
--c-text         → --readout #C7D0D9
--c-success → --glow-lime, --c-danger → --glow-red, --c-warning → --glow-amber
```

## Typography

**Monospace-forward.** A mono UI face (`'JetBrains Mono', 'SF Mono', Consolas, monospace`) for
labels, values, and readouts makes the whole UI read as an instrument. Body prose can drop to a
system sans for longer text to protect readability.

- **Wordmark "p3a":** bold mono or a 7-segment / dot-matrix display style — like a device readout.
- **Numbers everywhere** (counts, intervals, version, IP, signal) get the mono naturally and look
  like telemetry.

## Surface & treatment language

- **Modules** with 1px `--grid` borders and an optional inner glow; **corner ticks / brackets** on
  the focused module (HUD framing).
- **Status LEDs:** tiny colored dots with bloom for connection / channel / fault state — replaces
  today's text badges with something that reads at a glance.
- **Pixel-grid substrate:** a faint 1px grid behind everything (the display matrix).
- Optional **scanline overlay** at very low alpha — must stay subtle to avoid kitsch (ship it off by
  default / behind a "CRT" easter-egg toggle).
- The existing **PICO-8 monitor** and **OTA** pages finally have a home here — they belong in a
  console aesthetic.

## The two poles — "Exhibit mode"

For IIIF / museum viewing, **Exhibit mode** dims the glow, hides the grid and brackets, and drops to
a near-flat dark wall — high-res art on calm dark is exactly how good digital galleries present it.
The instrument personality recedes; the art stays lit. This keeps the sober pole satisfied without a
second design system.

## Motion personality

**Boot-up / terminal reveals.** Panels type-on or sweep in with a scanline; status LEDs blink on
state change; the hue-cycle spinner runs the wheel live. Snappy and mechanical (≤180 ms), with the
occasional deliberate "instrument warming up" flourish on first load.

## Component sketches

- **Primary button:** `--panel-3` fill, `--glow-cyan` 1px border + text; hover adds the cyan bloom;
  active inverts to solid cyan with ink text.
- **Pills:** module chips on the grid; category = which glow color outlines them (user amber, pin
  magenta, builtin cyan, utility dim gray). Active = filled glow + bloom.
- **Now-playing:** a telemetry strip — mono stats, a lime "ON AIR" LED, the hue-cycle spinner while
  refreshing.
- **Connection / cooldown banners:** become instrument alerts with the amber/red glow LEDs.

## Pros / cons

**Pros**
- Unmistakable, memorable personality; celebrates that p3a is real hardware.
- Dark + emissive flatters pixel art *and* IIIF scans.
- Mono readouts suit a device with lots of telemetry (signal, intervals, OTA, PICO-8).
- Strongest differentiation from every generic web UI.

**Cons / risks**
- "Gamer / techno" energy can clash with museum gravitas if Exhibit mode isn't disciplined.
- Glow + scanline texture is easy to overdo → looks like a skin, not a brand. Needs a light hand.
- All-mono UI hurts readability for longer text; reserve mono for labels/data, sans for prose.
