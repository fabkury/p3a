#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Makes a p3a font file from a pixel font's TTF.

    python tools/gen_font.py <font.ttf> <size> <out.p3afont> [--name "Family"]

`size` is the font's own pixel size (its "base glyph height" times whatever
makes it crisp; for the VEXED fonts, the number their Info file gives under
"Scales"). The TTF is rendered without anti-aliasing at exactly that size,
one bit a pixel; the firmware draws it at whole multiples.

The output format is described in components/p3a_ui/ui_font.h. Font files
are committed, so a build needs neither Pillow nor the TTFs; run this only
to add or change a font. `--check <file>` prints what a font file holds.

Characters: printable ASCII and the Latin-1 letters and signs the font has.
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

MAGIC = b"P3AF"
VERSION = 1
CODEPOINTS = list(range(0x20, 0x7F)) + list(range(0xA1, 0x100))


def build(ttf: Path, size: int) -> bytes:
    from PIL import Image, ImageDraw, ImageFont  # only needed to make a font

    font = ImageFont.truetype(str(ttf), size)
    font.fontmode = "1"
    ascent, descent = font.getmetrics()
    height = ascent + descent
    if not 1 <= height <= 255:
        raise SystemExit(f"a line would be {height} pixels high; at most 255")

    # What the font draws for a character it lacks, to leave those out.
    def bitmap(ch: str) -> tuple[int, bytes]:
        advance = max(1, round(font.getlength(ch)))
        if advance > 255:
            raise SystemExit(f"U+{ord(ch):04X} is {advance} pixels wide; at most 255")
        img = Image.new("1", (advance, height), 0)
        ImageDraw.Draw(img).text((0, 0), ch, font=font, fill=1)
        row_bytes = (advance + 7) // 8
        rows = bytearray()
        for y in range(height):
            for b in range(row_bytes):
                byte = 0
                for bit in range(8):
                    x = b * 8 + bit
                    if x < advance and img.getpixel((x, y)):
                        byte |= 0x80 >> bit
                rows.append(byte)
        return advance, bytes(rows)

    missing = bitmap("￿")
    glyphs = []
    for cp in CODEPOINTS:
        made = bitmap(chr(cp))
        if cp > 0x7E and made == missing:
            continue  # the font has no such letter
        glyphs.append((cp, *made))
    if not any(cp == ord("?") for cp, _a, _b in glyphs):
        raise SystemExit("the font has no '?', which stands in for missing characters")

    table_end = 10 + 8 * len(glyphs)
    table, bitmaps, offset = bytearray(), bytearray(), table_end
    for cp, advance, rows in glyphs:
        table += struct.pack("<HBBI", cp, advance, 0, offset)
        bitmaps += rows
        offset += len(rows)
    header = MAGIC + struct.pack("<BBBBH", VERSION, height, ascent, 0, len(glyphs))
    return bytes(header + table + bitmaps)


def describe(data: bytes) -> str:
    if data[:4] != MAGIC or data[4] != VERSION:
        raise SystemExit("not a p3a font file")
    height, baseline, _reserved, count = struct.unpack_from("<BBBH", data, 5)
    widths = [data[10 + 8 * i + 2] for i in range(count)]
    first, last = struct.unpack_from("<H", data, 10)[0], struct.unpack_from("<H", data, 10 + 8 * (count - 1))[0]
    return (f"{count} characters (U+{first:04X} to U+{last:04X}), line height {height}, baseline {baseline}, "
            f"widths {min(widths)} to {max(widths)}, {len(data)} bytes")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("ttf", nargs="?")
    parser.add_argument("size", nargs="?", type=int)
    parser.add_argument("out", nargs="?")
    parser.add_argument("--check", metavar="FILE", help="describe an existing font file and exit")
    args = parser.parse_args()
    if args.check:
        print(describe(Path(args.check).read_bytes()))
        return 0
    if not (args.ttf and args.size and args.out):
        parser.error("give a TTF, a size and an output file")
    data = build(Path(args.ttf), args.size)
    Path(args.out).write_bytes(data)
    print(f"{args.out}: {describe(data)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
