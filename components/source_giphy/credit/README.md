# Giphy's marks

The marks p3a draws in the band under Giphy's artworks (D101) when the
artwork leaves a band high enough: one for a dark background colour, one
for a light one.

| File | What |
|------|------|
| `powered-by-giphy-on-dark.png` | For dark backgrounds. From Giphy's attribution marks (`Static Logos/Small/Dark Backgrounds/Poweredby_100px-Black_VertLogo.png`), as Giphy gives it: 100 by 27 pixels with a clear border. Kept unchanged, to make the picture file again from. |
| `powered-by-giphy-on-light.png` | For light backgrounds. From the same set (`Static Logos/Small/Light Backgrounds/Poweredby_100px-White_VertLogo.png`), as Giphy gives it: the mark on a black plate, 101 by 36 pixels, with one column and one row of faint black at its edge. Kept unchanged. |
| `powered-by-giphy-on-dark.p3aimg` | The picture file compiled into the firmware (`components/p3a_ui/ui_image.h`): the PNG with the clear border cut away and nothing scaled, 91 by 24. |
| `powered-by-giphy-on-light.p3aimg` | The same for the other: the PNG laid on black, which makes the faint column and row black and leaves no pixel to mix, 101 by 36. Nothing scaled. |

The picture files are committed, so a build needs no Pillow. To make them
again, from the repository root:

```
python tools/gen_image.py --soft --crop components/source_giphy/credit/powered-by-giphy-on-dark.png components/source_giphy/credit/powered-by-giphy-on-dark.p3aimg
python tools/gen_image.py --soft --ground 000000 components/source_giphy/credit/powered-by-giphy-on-light.png components/source_giphy/credit/powered-by-giphy-on-light.p3aimg
```

`tests/host/ui/test_ui_image.c` checks the sizes of both sources' picture
files. The marks are Giphy's and are used here to credit Giphy, as its
terms ask; they are not covered by this repository's licence.
