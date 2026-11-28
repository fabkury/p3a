#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Makes a p3a UI picture file from a PNG.

    python tools/gen_image.py <picture.png> <out.p3aimg>
    python tools/gen_image.py --soft [--crop] [--ground RRGGBB] [--height N] <picture.png> <out.p3aimg>
    python tools/gen_image.py --check <file.p3aimg>

The output format is described in components/p3a_ui/ui_image.h: a ten-byte
header and the pixels as blue, green, red, alpha. An icon's pixel is either
drawn or not (alpha 128 and up is drawn), so give icons hard edges.

--soft keeps the PNG's alpha as it is, for a picture that is mixed with a
known ground (ui_image_draw_on): a source's mark under its artworks.
Before that, in this order: --crop cuts away the wholly clear rows and
columns around the picture; --ground lays the picture on that colour, which
leaves no pixel clear; --height scales it, keeping its shape, to that many
rows.

Picture files are committed beside their PNGs, so a build needs no Pillow;
run this only when a picture changes.
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

MAGIC = b"P3AI"
VERSION = 1
MAX_SIDE = 512


def build(png: Path, soft: bool = False, height: int = 0, crop: bool = False, ground: str = "") -> bytes:
    from PIL import Image  # only needed to make a picture

    img = Image.open(png).convert("RGBA")
    if crop:
        box = img.getchannel("A").getbbox()
        if box is None:
            raise SystemExit(f"{png}: every pixel is clear")
        img = img.crop(box)
    if ground:
        under = Image.new("RGBA", img.size, tuple(bytes.fromhex(ground.lstrip("#"))) + (255,))
        under.alpha_composite(img)
        img = under
    if height and height != img.height:
        # Scaled with the colours weighted by their alpha, so that a clear
        # pixel's colour does not bleed into the edge beside it.
        width = max(1, round(img.width * height / img.height))
        img = img.convert("RGBa").resize((width, height), Image.LANCZOS).convert("RGBA")
    if not (1 <= img.width <= MAX_SIDE and 1 <= img.height <= MAX_SIDE):
        raise SystemExit(f"{png}: {img.width}x{img.height}; a side may be 1 to {MAX_SIDE} pixels")
    out = bytearray(MAGIC + struct.pack("<BBHH", VERSION, 0, img.width, img.height))
    for r, g, b, a in getattr(img, "get_flattened_data", img.getdata)():  # the first is Pillow 12's name
        if soft:
            out += bytes((b, g, r, a) if a else (0, 0, 0, 0))
        else:
            out += bytes((b, g, r, 255 if a >= 128 else 0))
    return bytes(out)


def describe(data: bytes) -> str:
    if data[:4] != MAGIC or data[4] != VERSION or len(data) < 10:
        raise SystemExit("not a p3a picture file")
    width, height = struct.unpack_from("<HH", data, 6)
    if len(data) != 10 + width * height * 4:
        raise SystemExit("the file's length does not match its size")
    drawn = sum(1 for i in range(13, len(data), 4) if data[i] >= 128)
    mixed = sum(1 for i in range(13, len(data), 4) if 0 < data[i] < 255)
    soft = f", {mixed} of them part clear" if mixed else ""
    return f"{width}x{height}, {drawn} of {width * height} pixels drawn{soft}, {len(data)} bytes"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("png", nargs="?")
    parser.add_argument("out", nargs="?")
    parser.add_argument("--soft", action="store_true", help="keep the PNG's alpha, for a picture mixed with a ground")
    parser.add_argument("--crop", action="store_true", help="cut away the clear rows and columns around the picture")
    parser.add_argument("--ground", default="", metavar="RRGGBB", help="lay the picture on this colour")
    parser.add_argument("--height", type=int, default=0, metavar="N", help="scale to N rows first, keeping the shape")
    parser.add_argument("--check", metavar="FILE", help="describe an existing picture file and exit")
    args = parser.parse_args()
    if args.check:
        print(describe(Path(args.check).read_bytes()))
        return 0
    if not (args.png and args.out):
        parser.error("give a PNG and an output file")
    data = build(Path(args.png), args.soft, args.height, args.crop, args.ground)
    Path(args.out).write_bytes(data)
    print(f"{args.out}: {describe(data)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
