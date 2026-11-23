# Built-in fonts

Pixel fonts for the on-device UI, drawn at whole multiples of their own
size (`../ui_font.h` describes the file format).

| File | Font | Size | Author | Licence |
|---|---|---|---|---|
| `ui.p3afont` | Everyday Standard | 6 px | [VEXED](https://v3x3d.itch.io/) | [Creative Commons Attribution 4.0 International](https://creativecommons.org/licenses/by/4.0/) |

The font files are made from the TTFs by `tools/gen_font.py` and committed,
so a build needs neither the TTFs nor Pillow:

```
python tools/gen_font.py <Everyday_Standard.ttf> 6 components/p3a_ui/fonts/ui.p3afont
python tools/gen_font.py --check components/p3a_ui/fonts/ui.p3afont
```

The glyphs are unchanged from the TTF; only the container differs. The
licence asks for attribution: this table is it, and it is repeated wherever
the firmware lists its third-party parts.

The choice of family is provisional until `docs/2.0/DECISIONS.md` O4 is
decided. Changing it is one command and the screenshot references.
