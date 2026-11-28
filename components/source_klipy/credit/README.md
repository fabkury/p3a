# Klipy's mark

The mark p3a draws in the band under Klipy's artworks (D101), at the right,
when the artwork leaves a band high enough for it.

| File | What |
|------|------|
| `powered-by-klipy.png` | The mark from Klipy's logos for a GIF picker (`PNG Files/Powered by KLIPY Horizontal - Yellow&White Logo.png`), as Klipy gives it: 640 by 137 pixels. Kept unchanged, to make the picture file again from. |
| `powered-by-klipy.p3aimg` | The picture file compiled into the firmware (`components/p3a_ui/ui_image.h`), made from the PNG with the clear border cut away and the rest scaled to the height of Giphy's mark, 24 rows, which makes it 142 wide. Committed, so a build needs no Pillow. |

To make the picture file again, from the repository root:

```
python tools/gen_image.py --soft --crop --height 24 components/source_klipy/credit/powered-by-klipy.png components/source_klipy/credit/powered-by-klipy.p3aimg
```

`tests/host/ui/test_ui_image.c` checks that both sources' picture files open
and are the same height. The mark is Klipy's and is used here to credit Klipy,
as its terms ask; it is not covered by this repository's licence.
