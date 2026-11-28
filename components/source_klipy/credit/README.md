# Klipy's marks

The marks p3a draws in the band under Klipy's artworks (D101) when the
artwork leaves a band high enough: one for a dark background colour, one
for a light one.

| File | What |
|------|------|
| `powered-by-klipy-on-dark.png` | For dark backgrounds. From Klipy's logos for a GIF picker (`PNG Files/Powered by KLIPY Horizontal - Yellow&White Logo.png`), as Klipy gives it: 640 by 137 pixels. Kept unchanged, to make the picture file again from. |
| `powered-by-klipy-on-light.png` | For light backgrounds. From the same set (`PNG Files/Powered by KLIPY Horizontal - Yellow&Black Logo.png`), as Klipy gives it: 640 by 137 pixels. Kept unchanged. |
| `powered-by-klipy-on-dark.p3aimg`, `powered-by-klipy-on-light.p3aimg` | The picture files compiled into the firmware (`components/p3a_ui/ui_image.h`): each PNG with the clear border cut away and the rest scaled to the height of Giphy's mark, 24 rows, which makes it 142 wide. |

The picture files are committed, so a build needs no Pillow. To make them
again, from the repository root:

```
python tools/gen_image.py --soft --crop --height 24 components/source_klipy/credit/powered-by-klipy-on-dark.png components/source_klipy/credit/powered-by-klipy-on-dark.p3aimg
python tools/gen_image.py --soft --crop --height 24 components/source_klipy/credit/powered-by-klipy-on-light.png components/source_klipy/credit/powered-by-klipy-on-light.p3aimg
```

`tests/host/ui/test_ui_image.c` checks the sizes of both sources' picture
files. The marks are Klipy's and are used here to credit Klipy, as its
terms ask; they are not covered by this repository's licence.
