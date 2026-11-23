# Built-in fonts

Pixel fonts for the on-device UI, drawn at whole multiples of their own
size (`../ui_font.h` describes the file format).

| File | Font | Size | Author | Licence |
|---|---|---|---|---|
| `ui.p3afont` | Everyday Ample | 9 px | [VEXED](https://v3x3d.itch.io/) | [Creative Commons Attribution 4.0 International](https://creativecommons.org/licenses/by/4.0/) |

The font files are made from the TTFs by `tools/gen_font.py` and committed,
so a build needs neither the TTFs nor Pillow:

```
python tools/gen_font.py <Everyday_Ample.ttf> 9 components/p3a_ui/fonts/ui.p3afont
python tools/gen_font.py --check components/p3a_ui/fonts/ui.p3afont
```

The glyphs are unchanged from the TTF; only the container differs. The
licence asks for attribution: this table is it, and it is repeated wherever
the firmware lists its third-party parts.

The family was chosen by Fab (`docs/2.0/DECISIONS.md` D61). Changing it is
one command; the sizes are the scales in `ui_style_default()`.
