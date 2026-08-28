#!/usr/bin/env python3
"""make_test_anim.py -- synthetic test animations for provocation runs (jitter work stream).

    python host/jitter-lab/make_test_anim.py [--out host/jitter-lab/runs/test-anims] [--fps 30] [--size 240]

Produces GIF (and WebP when Pillow can write animated WebP) files with a
vertical bar sweeping left to right plus a frame counter strip, at a fixed
frame delay. Decode cost is tiny and constant, so any lateness is a stall
by construction (no producer overrun possible). Upload with:

    curl -F "file=@bar_30fps.gif" http://p3a.local/upload
"""
import argparse
from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent


def make(out_dir: Path, fps: int, size: int, seconds: int = 4):
    n = fps * seconds
    delay_ms = round(1000 / fps)
    frames = []
    for i in range(n):
        im = Image.new("RGB", (size, size), (8, 8, 16))
        d = ImageDraw.Draw(im)
        x = int((i / n) * (size - 12))
        d.rectangle([x, 0, x + 12, size - 1], fill=(255, 255, 255))
        # frame counter strip: 8 binary blocks along the bottom
        for b in range(8):
            on = (i >> b) & 1
            d.rectangle([b * (size // 8), size - 10, (b + 1) * (size // 8) - 2, size - 1],
                        fill=(255, 200, 0) if on else (40, 40, 40))
        frames.append(im)
    out_dir.mkdir(parents=True, exist_ok=True)
    gif = out_dir / f"bar_{fps}fps.gif"
    frames[0].save(gif, save_all=True, append_images=frames[1:], duration=delay_ms, loop=0, optimize=False)
    print("wrote", gif, f"({n} frames @ {delay_ms} ms)")
    try:
        webp = out_dir / f"bar_{fps}fps.webp"
        frames[0].save(webp, save_all=True, append_images=frames[1:], duration=delay_ms, loop=0, lossless=True)
        print("wrote", webp)
    except Exception as e:  # noqa: BLE001
        print("webp skipped:", e)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(HERE / "runs" / "test-anims"))
    ap.add_argument("--fps", type=int, nargs="*", default=[30, 25, 60])
    ap.add_argument("--size", type=int, default=240)
    a = ap.parse_args()
    for fps in a.fps:
        make(Path(a.out), fps, a.size)


if __name__ == "__main__":
    main()
