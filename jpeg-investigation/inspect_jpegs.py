"""Inspect JPEG files to identify why each fails on the ESP32 HW JPEG decoder.

For each JPEG, prints: filename, width, height, file size, mode, progressive,
chroma subsampling, EXIF orientation, divisibility by 8/16, and an estimate
of the RGB buffer size the decoder would allocate (W*H*3 bytes).

Usage:
    python inspect_jpegs.py [file_or_dir ...]

If no args, scans ./animations/ relative to this script.
"""
from __future__ import annotations

import os
import struct
import sys
from pathlib import Path

from PIL import Image, ImageFile

ImageFile.LOAD_TRUNCATED_IMAGES = False

SUBSAMPLING_NAMES = {
    0: "4:4:4",
    1: "4:2:2",
    2: "4:2:0",
    -1: "unknown",
}


def parse_sof_via_raw(path: Path) -> tuple[int, int, str] | None:
    """Walk JPEG markers manually and return (width, height, sof_marker_name).
    Returns None if no SOF marker is found (suggests structural corruption).
    """
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as exc:
        return None

    if len(data) < 4 or data[0:2] != b"\xff\xd8":
        return None

    i = 2
    while i < len(data) - 1:
        if data[i] != 0xFF:
            return None
        # skip fill bytes
        while i < len(data) and data[i] == 0xFF:
            i += 1
        if i >= len(data):
            return None
        marker = data[i]
        i += 1

        # Standalone markers
        if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
            continue

        if i + 2 > len(data):
            return None
        seg_len = struct.unpack(">H", data[i:i + 2])[0]
        if seg_len < 2 or i + seg_len > len(data):
            return None

        # SOF markers: C0..CF except C4 (DHT), C8 (reserved), CC (DAC)
        if 0xC0 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
            if seg_len < 8:
                return None
            # P (1) | Y (2) | X (2) | Nf (1)
            height = struct.unpack(">H", data[i + 3:i + 5])[0]
            width = struct.unpack(">H", data[i + 5:i + 7])[0]
            return width, height, f"SOF{marker - 0xC0:X}"

        if marker == 0xDA:  # SOS — entropy data follows; stop parsing markers
            return None
        i += seg_len

    return None


def inspect_one(path: Path) -> dict:
    info = {
        "name": path.name,
        "size": path.stat().st_size,
        "pil_w": None,
        "pil_h": None,
        "raw_w": None,
        "raw_h": None,
        "sof": None,
        "progressive": None,
        "subsampling": None,
        "mode": None,
        "exif_orientation": None,
        "error": None,
    }

    raw = parse_sof_via_raw(path)
    if raw is not None:
        info["raw_w"], info["raw_h"], info["sof"] = raw

    try:
        with Image.open(path) as img:
            info["pil_w"], info["pil_h"] = img.size
            info["mode"] = img.mode
            info["progressive"] = bool(img.info.get("progressive", False))
            ss = img.info.get("subsampling", -1)
            if isinstance(ss, int):
                info["subsampling"] = SUBSAMPLING_NAMES.get(ss, f"raw={ss}")
            else:
                info["subsampling"] = str(ss)
            try:
                exif = img.getexif()
                info["exif_orientation"] = exif.get(0x0112)
            except Exception:
                pass
    except Exception as exc:
        info["error"] = f"{type(exc).__name__}: {exc}"

    return info


def fmt(info: dict) -> str:
    w = info["pil_w"] if info["pil_w"] is not None else info["raw_w"]
    h = info["pil_h"] if info["pil_h"] is not None else info["raw_h"]

    rgb_bytes = (w * h * 3) if (w and h) else None
    rgb_str = f"{rgb_bytes:>10d}" if rgb_bytes is not None else "         -"

    div8 = "         -"
    div16 = "          -"
    if w and h:
        div8 = f"w%8={w%8} h%8={h%8}"
        div16 = f"w%16={w%16} h%16={h%16}"

    sof = info["sof"] or "-"
    prog = "yes" if info["progressive"] else ("no" if info["progressive"] is False else "-")
    ss = info["subsampling"] or "-"
    mode = info["mode"] or "-"
    err = info["error"] or ""
    exif = info["exif_orientation"] if info["exif_orientation"] else "-"

    return (
        f"{info['name']:<40} "
        f"{str(w or '-'):>5}x{str(h or '-'):<5} "
        f"size={info['size']:>9d} "
        f"rgb={rgb_str} "
        f"sof={sof:<5} "
        f"prog={prog:<3} "
        f"ss={ss:<8} "
        f"mode={mode:<5} "
        f"orient={exif} "
        f"{div8} {div16}"
        + (f"  ERR={err}" if err else "")
    )


def collect(args: list[str]) -> list[Path]:
    if not args:
        here = Path(__file__).parent
        return sorted(here.joinpath("animations").glob("*.jp*g"))

    out: list[Path] = []
    for a in args:
        p = Path(a)
        if p.is_dir():
            out.extend(sorted(p.glob("*.jp*g")))
        elif p.exists():
            out.append(p)
    return out


def main(argv: list[str]) -> int:
    paths = collect(argv[1:])
    if not paths:
        print("No JPEGs found.", file=sys.stderr)
        return 1

    print(f"# Inspecting {len(paths)} JPEG file(s)\n")
    rows = [inspect_one(p) for p in paths]
    for r in rows:
        print(fmt(r))

    # Summary: any without SOF markers, any progressive, any wide/tall
    print("\n# Summary")
    no_sof = [r for r in rows if r["sof"] is None and not r["error"]]
    progressive = [r for r in rows if r["progressive"]]
    huge = [r for r in rows if (r["pil_w"] or 0) * (r["pil_h"] or 0) * 3 > 5_000_000]
    not_div8_w = [r for r in rows if (r["pil_w"] or 0) % 8 != 0]
    not_div16_w = [r for r in rows if (r["pil_w"] or 0) % 16 != 0]
    not_div8_h = [r for r in rows if (r["pil_h"] or 0) % 8 != 0]
    not_div16_h = [r for r in rows if (r["pil_h"] or 0) % 16 != 0]

    print(f"  No SOF marker found            : {len(no_sof)}  {[r['name'] for r in no_sof]}")
    print(f"  Progressive JPEGs              : {len(progressive)}  {[r['name'] for r in progressive]}")
    print(f"  >5 MB RGB buffer (W*H*3 > 5MB) : {len(huge)}  {[r['name'] for r in huge]}")
    print(f"  Width  not divisible by 8      : {len(not_div8_w)}")
    print(f"  Width  not divisible by 16     : {len(not_div16_w)}")
    print(f"  Height not divisible by 8      : {len(not_div8_h)}")
    print(f"  Height not divisible by 16     : {len(not_div16_h)}")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
