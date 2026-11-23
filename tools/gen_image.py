#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Makes a p3a UI picture file from a PNG.

    python tools/gen_image.py <picture.png> <out.p3aimg>
    python tools/gen_image.py --check <file.p3aimg>

The output format is described in components/p3a_ui/ui_image.h: a ten-byte
header and the pixels as blue, green, red, alpha. A pixel is either drawn or
not (alpha 128 and up is drawn), so give icons hard edges.

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


def build(png: Path) -> bytes:
    from PIL import Image  # only needed to make a picture

    img = Image.open(png).convert("RGBA")
    if not (1 <= img.width <= MAX_SIDE and 1 <= img.height <= MAX_SIDE):
        raise SystemExit(f"{png}: {img.width}x{img.height}; a side may be 1 to {MAX_SIDE} pixels")
    out = bytearray(MAGIC + struct.pack("<BBHH", VERSION, 0, img.width, img.height))
    for r, g, b, a in getattr(img, "get_flattened_data", img.getdata)():  # the first is Pillow 12's name
        out += bytes((b, g, r, 255 if a >= 128 else 0))
    return bytes(out)


def describe(data: bytes) -> str:
    if data[:4] != MAGIC or data[4] != VERSION or len(data) < 10:
        raise SystemExit("not a p3a picture file")
    width, height = struct.unpack_from("<HH", data, 6)
    if len(data) != 10 + width * height * 4:
        raise SystemExit("the file's length does not match its size")
    drawn = sum(1 for i in range(13, len(data), 4) if data[i] >= 128)
    return f"{width}x{height}, {drawn} of {width * height} pixels drawn, {len(data)} bytes"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("png", nargs="?")
    parser.add_argument("out", nargs="?")
    parser.add_argument("--check", metavar="FILE", help="describe an existing picture file and exit")
    args = parser.parse_args()
    if args.check:
        print(describe(Path(args.check).read_bytes()))
        return 0
    if not (args.png and args.out):
        parser.error("give a PNG and an output file")
    data = build(Path(args.png))
    Path(args.out).write_bytes(data)
    print(f"{args.out}: {describe(data)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
