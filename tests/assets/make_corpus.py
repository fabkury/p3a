#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generates the decoder test corpus in tests/assets/decoders/.

    python tests/assets/make_corpus.py

Small, synthetic, deterministic files: a few of every kind each decoder has
to handle, and a set of damaged ones that every decoder has to refuse without
harm. The files are committed; run this again only to add a case, and commit
what changes. Needs Pillow.

good/<format>/<name>.<ext>   decodes; the device tests compare the pixels
                             with Pillow's decoding of the same file
bad/<format>/<name>.<ext>    must be refused, or stop cleanly part-way
"""
from __future__ import annotations

import io
import struct
import zlib
from pathlib import Path

from PIL import Image, ImageDraw

OUT = Path(__file__).resolve().parent / "decoders"


def scene(w: int, h: int, phase: int = 0, alpha: bool = False) -> Image.Image:
    """A picture with gradients, hard edges and, if asked, soft transparency."""
    img = Image.new("RGBA" if alpha else "RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            r = (x * 255) // max(1, w - 1)
            g = (y * 255) // max(1, h - 1)
            b = ((x + y) * 7 + phase * 40) & 0xFF
            if alpha:
                dx, dy = x - w / 2, y - h / 2
                dist = (dx * dx + dy * dy) ** 0.5
                a = max(0, min(255, int(255 - dist * 510 / max(w, h))))
                px[x, y] = (r, g, b, a)
            else:
                px[x, y] = (r, g, b)
    draw = ImageDraw.Draw(img)
    box = (w // 8 + phase * 3, h // 8, w // 8 + phase * 3 + w // 4, h // 8 + h // 4)
    draw.rectangle(box, fill=(255, 40, 40, 255) if alpha else (255, 40, 40))
    if w >= 16 and h >= 16:
        draw.ellipse((w // 2, h // 2, w - 3, h - 3), outline=(255, 255, 255, 255) if alpha else (255, 255, 255))
    return img


def sprite_frames(w: int, h: int, count: int) -> list[Image.Image]:
    """Animation frames on a fully transparent ground: a block moving across."""
    frames = []
    for i in range(count):
        img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        draw = ImageDraw.Draw(img)
        x = 2 + i * (w - 14) // max(1, count - 1)
        draw.rectangle((x, h // 3, x + 10, h // 3 + 10), fill=(250, 200 - i * 30, 20 + i * 50, 255))
        draw.ellipse((w - 14, 2, w - 3, 13), fill=(30, 140, 255, 160))
        frames.append(img)
    return frames


def save(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def encoded(img: Image.Image, fmt: str, **options) -> bytes:
    buf = io.BytesIO()
    img.save(buf, fmt, **options)
    return buf.getvalue()


def png_chunk(kind: bytes, body: bytes) -> bytes:
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)


def good() -> dict[str, bytes]:
    files: dict[str, bytes] = {}

    # --- PNG and APNG
    files["png/rgb.png"] = encoded(scene(48, 32), "PNG")
    files["png/rgba_soft_alpha.png"] = encoded(scene(40, 40, alpha=True), "PNG")
    files["png/palette.png"] = encoded(scene(32, 32).convert("P", palette=Image.ADAPTIVE, colors=16), "PNG")
    pal = scene(32, 32, alpha=True).convert("P", palette=Image.ADAPTIVE, colors=15)
    files["png/palette_transparent.png"] = encoded(pal, "PNG", transparency=0)
    files["png/gray.png"] = encoded(scene(32, 24).convert("L"), "PNG")
    files["png/gray_alpha.png"] = encoded(scene(32, 24, alpha=True).convert("LA"), "PNG")
    gray = scene(24, 24).convert("L")
    deep = Image.new("I;16", gray.size)
    deep.putdata([v * 257 for v in gray.getdata()])  # 8-bit values spread over the 16-bit range
    files["png/gray16.png"] = encoded(deep, "PNG")
    files["png/one_pixel.png"] = encoded(Image.new("RGB", (1, 1), (12, 200, 99)), "PNG")
    files["png/wide.png"] = encoded(scene(200, 3), "PNG")
    frames = sprite_frames(40, 40, 5)
    files["png/apng_sprite.png"] = encoded(frames[0], "PNG", save_all=True, append_images=frames[1:],
                                           duration=[100, 50, 200, 100, 70], loop=0, disposal=1, blend=0)
    files["png/apng_over.png"] = encoded(frames[0], "PNG", save_all=True, append_images=frames[1:4],
                                         duration=80, loop=0, disposal=0, blend=1)
    opaque = [scene(36, 28, phase=i) for i in range(4)]
    # Delays that browsers do not believe (0 and 10 ms) next to ones they do.
    files["png/apng_short_delays.png"] = encoded(opaque[0], "PNG", save_all=True, append_images=opaque[1:],
                                                 duration=[0, 10, 20, 100], loop=0)
    files["png/apng_opaque.png"] = encoded(opaque[0], "PNG", save_all=True, append_images=opaque[1:],
                                           duration=[40, 40, 120, 40], loop=0)
    # Files that say how many times to play. p3a honours it for APNG (the last
    # frame is then held) and plays GIF and WebP for ever whatever they say.
    files["png/apng_plays_twice.png"] = encoded(opaque[0], "PNG", save_all=True, append_images=opaque[1:],
                                                duration=[40, 40, 120, 40], loop=2)

    # --- GIF
    files["gif/still.gif"] = encoded(scene(48, 32).convert("P", palette=Image.ADAPTIVE), "GIF")
    files["gif/opaque_anim.gif"] = encoded(opaque[0], "GIF", save_all=True, append_images=opaque[1:],
                                           duration=[40, 80, 120, 200], loop=0)
    files["gif/plays_once.gif"] = encoded(opaque[0], "GIF", save_all=True, append_images=opaque[1:],
                                          duration=[40, 80, 120, 200], loop=1)
    for disposal in (1, 2, 3):
        files[f"gif/sprite_disposal{disposal}.gif"] = encoded(
            frames[0], "GIF", save_all=True, append_images=frames[1:], duration=[100, 60, 60, 60, 150], loop=0,
            disposal=disposal, transparency=0)
    files["gif/short_delays.gif"] = encoded(opaque[0], "GIF", save_all=True, append_images=opaque[1:],
                                            duration=[0, 10, 20, 100], loop=0)
    files["gif/one_pixel.gif"] = encoded(Image.new("P", (1, 1), 0), "GIF")

    # --- WebP
    files["webp/lossless.webp"] = encoded(scene(48, 32), "WEBP", lossless=True)
    files["webp/lossless_alpha.webp"] = encoded(scene(40, 40, alpha=True), "WEBP", lossless=True, exact=True)
    files["webp/lossy.webp"] = encoded(scene(64, 48), "WEBP", quality=85)
    files["webp/lossy_alpha.webp"] = encoded(scene(48, 48, alpha=True), "WEBP", quality=85)
    files["webp/anim_lossless_alpha.webp"] = encoded(frames[0], "WEBP", save_all=True, append_images=frames[1:],
                                                     duration=[100, 50, 200, 100, 70], loop=0, lossless=True,
                                                     exact=True)
    files["webp/anim_opaque.webp"] = encoded(opaque[0], "WEBP", save_all=True, append_images=opaque[1:],
                                             duration=[40, 40, 120, 40], loop=0, lossless=True)
    files["webp/plays_once.webp"] = encoded(opaque[0], "WEBP", save_all=True, append_images=opaque[1:],
                                            duration=[40, 40, 120, 40], loop=1, lossless=True)
    files["webp/anim_short_delays.webp"] = encoded(opaque[0], "WEBP", save_all=True, append_images=opaque[1:],
                                                   duration=[0, 10, 20, 100], loop=0, lossless=True)
    files["webp/anim_lossy.webp"] = encoded(opaque[0], "WEBP", save_all=True, append_images=opaque[1:],
                                            duration=60, loop=0, quality=80)

    # --- JPEG. "baseline" files go to the hardware decoder; the rest make it
    # refuse, which exercises the software one.
    files["jpeg/baseline.jpg"] = encoded(scene(64, 48), "JPEG", quality=90)
    files["jpeg/baseline_odd_size.jpg"] = encoded(scene(33, 17), "JPEG", quality=90)
    files["jpeg/baseline_444.jpg"] = encoded(scene(48, 48), "JPEG", quality=92, subsampling=0)
    files["jpeg/progressive.jpg"] = encoded(scene(64, 48), "JPEG", quality=90, progressive=True)
    files["jpeg/gray.jpg"] = encoded(scene(48, 32).convert("L"), "JPEG", quality=90)
    files["jpeg/larger_than_screen.jpg"] = encoded(scene(1440, 1440), "JPEG", quality=60, progressive=True)

    # --- BMP
    files["bmp/rgb24.bmp"] = encoded(scene(48, 32), "BMP")
    files["bmp/rgba32.bmp"] = encoded(scene(40, 40, alpha=True), "BMP")
    files["bmp/palette8.bmp"] = encoded(scene(32, 32).convert("P", palette=Image.ADAPTIVE, colors=64), "BMP")
    files["bmp/mono1.bmp"] = encoded(scene(33, 9).convert("1"), "BMP")
    files["bmp/odd_width.bmp"] = encoded(scene(31, 5), "BMP")
    return files


def bad(ok: dict[str, bytes]) -> dict[str, bytes]:
    files: dict[str, bytes] = {}
    sources = {"png": "png/rgba_soft_alpha.png", "apng": "png/apng_sprite.png", "gif": "gif/sprite_disposal2.gif",
               "webp": "webp/lossless_alpha.webp", "webp_anim": "webp/anim_lossless_alpha.webp",
               "jpeg": "jpeg/baseline.jpg", "jpeg_prog": "jpeg/progressive.jpg", "bmp": "bmp/rgb24.bmp"}
    for label, source in sources.items():
        data = ok[source]
        fmt, ext = source.split("/")[0], source.rsplit(".", 1)[1]
        files[f"{fmt}/{label}_truncated_half.{ext}"] = data[:len(data) // 2]
        files[f"{fmt}/{label}_truncated_header.{ext}"] = data[:20]
        # The same file with a run of bytes in its middle overwritten.
        mid = len(data) // 2
        files[f"{fmt}/{label}_damaged_middle.{ext}"] = data[:mid] + bytes([0xA5]) * 24 + data[mid + 24:]
    for fmt, ext in (("png", "png"), ("gif", "gif"), ("webp", "webp"), ("jpeg", "jpg"), ("bmp", "bmp")):
        files[f"{fmt}/empty_ish.{ext}"] = b"\x00"
        files[f"{fmt}/not_an_image.{ext}"] = b"This is not an image. " * 8

    # Files whose headers claim sizes that must not be believed.
    ihdr = struct.pack(">IIBBBBB", 60000, 60000, 8, 6, 0, 0, 0)
    files["png/claims_60000x60000.png"] = (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", ihdr) +
                                           png_chunk(b"IDAT", zlib.compress(b"\x00" * 64)) + png_chunk(b"IEND", b""))
    ihdr = struct.pack(">IIBBBBB", 5000, 16, 8, 2, 0, 0, 0)
    files["png/claims_5000_wide.png"] = (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", ihdr) +
                                         png_chunk(b"IDAT", zlib.compress(b"\x00" * 64)) + png_chunk(b"IEND", b""))
    gif = bytearray(ok["gif/still.gif"])
    gif[6:10] = struct.pack("<HH", 65535, 65535)  # the logical screen
    files["gif/claims_65535x65535.gif"] = bytes(gif)
    jpeg = bytearray(ok["jpeg/baseline.jpg"])
    sof = jpeg.index(b"\xff\xc0")
    jpeg[sof + 5:sof + 9] = struct.pack(">HH", 65535, 65535)  # height, width
    files["jpeg/claims_65535x65535.jpg"] = bytes(jpeg)
    jpeg = bytearray(ok["jpeg/progressive.jpg"])
    sof = jpeg.index(b"\xff\xc2")
    jpeg[sof + 5:sof + 9] = struct.pack(">HH", 65535, 65535)
    files["jpeg/progressive_claims_65535x65535.jpg"] = bytes(jpeg)
    bmp = bytearray(ok["bmp/rgb24.bmp"])
    bmp[18:26] = struct.pack("<ii", 100000, 100000)
    files["bmp/claims_100000x100000.bmp"] = bytes(bmp)
    bmp = bytearray(ok["bmp/rgb24.bmp"])
    bmp[18:26] = struct.pack("<ii", 4000, 4000)  # a plausible size the file does not hold the pixels for
    files["bmp/claims_more_pixels_than_it_has.bmp"] = bytes(bmp)
    return files


def main() -> None:
    ok = good()
    for name, data in ok.items():
        save(OUT / "good" / name, data)
    broken = bad(ok)
    for name, data in broken.items():
        save(OUT / "bad" / name, data)
    total = sum(len(d) for d in ok.values()) + sum(len(d) for d in broken.values())
    print(f"{len(ok)} good and {len(broken)} bad files, {total} bytes, in {OUT}")


if __name__ == "__main__":
    main()
