# Giphy's mark

The mark p3a draws in the band under Giphy's artworks (D101), at the right,
when the artwork leaves a band high enough for it.

| File | What |
|------|------|
| `powered-by-giphy.png` | The mark from Giphy's attribution marks (`Static Logos/Small/Dark Backgrounds/Poweredby_100px-Black_VertLogo.png`), as Giphy gives it: 100 by 27 pixels. Kept unchanged, to make the picture file again from. |
| `powered-by-giphy.p3aimg` | The picture file compiled into the firmware (`components/p3a_ui/ui_image.h`), made from the PNG. Committed, so a build needs no Pillow. |

To make the picture file again, from the repository root:

```
python tools/gen_image.py --soft components/source_giphy/credit/powered-by-giphy.png components/source_giphy/credit/powered-by-giphy.p3aimg
```

`tests/host/ui/test_ui_image.c` checks that both sources' picture files open
and are the same height. The mark is Giphy's and is used here to credit Giphy,
as its terms ask; it is not covered by this repository's licence.
