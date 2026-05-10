"""Inspect JPEG files to identify why each fails on the ESP32 HW JPEG decoder.

For each JPEG, prints: filename, width, height, file size, mode, progressive,
chroma subsampling, EXIF orientation, the (W*H) % 8 value (this is the actual
IDF v5.5.1 SOF gate from `esp_driver_jpeg/jpeg_parse_marker.c:106` — files
where (W*H) % 8 != 0 are rejected with `ESP_ERR_INVALID_STATE`), per-dimension
divisibility by 8/16 for context, an estimate of the RGB buffer size the HW
decoder would allocate (W*H*3 bytes), and an estimate of the DCT coefficient
buffer size that libjpeg-turbo would need for a progressive decode.

The coefficient-buffer estimate matters because libjpeg-turbo's scaled IDCT
shrinks the *output* buffer but not the per-component coefficient buffer:
progressive decode requires the full-resolution DCT coefficients resident
across all scans. With ~32 MiB PSRAM on the ESP32-P4-WIFI6-Touch-LCD-4B
(minus what is already pinned for double-buffering and other allocations),
that is the binding constraint for the largest progressive files, not the
RGB output buffer.

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

# Bytes of DCT coefficient storage per source pixel for libjpeg-turbo's
# progressive decode path, by chroma subsampling. Each coefficient is int16
# (JCOEF). Y is full-res; Cb/Cr are sampled at the subsampling ratio. Totals:
#   4:4:4 -> 3 components * 1.0 * 2 bytes = 6
#   4:2:2 -> Y full + Cb/Cr at 1/2 width    = 2 * (1 + 0.5 + 0.5) = 4
#   4:2:0 -> Y full + Cb/Cr at 1/4 area     = 2 * (1 + 0.25 + 0.25) = 3
COEFF_BYTES_PER_PIXEL = {
    "4:4:4": 6,
    "4:2:2": 4,
    "4:2:0": 3,
}


def coeff_buf_bytes(w, h, subsampling):
    """Estimate the libjpeg-turbo DCT coefficient buffer for a progressive
    decode of a JPEG of size w*h with the given chroma subsampling string.
    Returns None if dimensions are missing. Falls back to 4:2:0 (the dominant
    web encoding, and the more optimistic estimate) when subsampling is
    unknown.
    """
    if not w or not h:
        return None
    bpp = COEFF_BYTES_PER_PIXEL.get(subsampling or "", COEFF_BYTES_PER_PIXEL["4:2:0"])
    return w * h * bpp


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

    coeff_bytes = coeff_buf_bytes(w, h, info["subsampling"])
    coeff_str = f"{coeff_bytes:>10d}" if coeff_bytes is not None else "         -"

    wh_mod8 = "          -"
    div8 = "         -"
    div16 = "          -"
    if w and h:
        wh_mod8 = f"(w*h)%8={(w*h)%8}"
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
        f"coeff={coeff_str} "
        f"sof={sof:<5} "
        f"prog={prog:<3} "
        f"ss={ss:<8} "
        f"mode={mode:<5} "
        f"orient={exif} "
        f"{wh_mod8} {div8} {div16}"
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

    # Summary
    # The (W*H)%8 row is the actual IDF v5.5.1 SOF gate (Issue C in the epic);
    # files in that set are rejected by the HW decoder. The per-dimension
    # divisibility rows are kept as informational context — height misalignment
    # alone is tolerated by the silicon, and width misalignment alone is
    # tolerated by the wrapper post-`732475d4` (which rounds the output buffer
    # up to 16 — Issue D, fixed).
    print("\n# Summary")
    no_sof = [r for r in rows if r["sof"] is None and not r["error"]]
    progressive = [r for r in rows if r["progressive"]]
    huge = [r for r in rows if (r["pil_w"] or 0) * (r["pil_h"] or 0) * 3 > 5_000_000]
    bad_wh_mod8 = [r for r in rows
                   if r["pil_w"] and r["pil_h"]
                   and (r["pil_w"] * r["pil_h"]) % 8 != 0]
    not_div8_w = [r for r in rows if (r["pil_w"] or 0) % 8 != 0]
    not_div16_w = [r for r in rows if (r["pil_w"] or 0) % 16 != 0]
    not_div8_h = [r for r in rows if (r["pil_h"] or 0) % 8 != 0]
    not_div16_h = [r for r in rows if (r["pil_h"] or 0) % 16 != 0]

    print(f"  No SOF marker found              : {len(no_sof)}  {[r['name'] for r in no_sof]}")
    print(f"  Progressive JPEGs (Issue B)      : {len(progressive)}  {[r['name'] for r in progressive]}")
    print(f"  >5 MB RGB buffer (Issue A est.)  : {len(huge)}  {[r['name'] for r in huge]}")
    print(f"  (W*H) % 8 != 0 (Issue C, v5.5.1) : {len(bad_wh_mod8)}  {[r['name'] for r in bad_wh_mod8]}")
    print(f"  Width  not divisible by 8  (info): {len(not_div8_w)}")
    print(f"  Width  not divisible by 16 (info): {len(not_div16_w)}")
    print(f"  Height not divisible by 8  (info): {len(not_div8_h)}")
    print(f"  Height not divisible by 16 (info): {len(not_div16_h)}")

    # Critical intersection for the SW-fallback design: any file that is BOTH
    # progressive (Issue B) AND oversized (Issue A) cannot be decoded by the
    # HW path (no progressive support) and also stresses libjpeg-turbo's full-
    # resolution coefficient buffer because progressive decode cannot leverage
    # scaled IDCT to shrink it. Such files would need a graceful-skip path.
    prog_and_huge = [r for r in progressive
                     if (r["pil_w"] or 0) * (r["pil_h"] or 0) * 3 > 5_000_000]
    print(f"\n  Progressive AND >5MB RGB (A & B) : {len(prog_and_huge)}  "
          f"{[r['name'] for r in prog_and_huge]}")

    # libjpeg-turbo coefficient-buffer headroom check. The board has ~32 MiB
    # PSRAM minus what is already pinned for native_frame_b1/b2 and other
    # allocations; treat 16 MiB as a comfort threshold and 24 MiB as a hard
    # warning band. These numbers are estimates, not measurements.
    big_coeff_16 = [r for r in progressive
                    if (coeff_buf_bytes(r["pil_w"], r["pil_h"], r["subsampling"]) or 0) > 16 * 1024 * 1024]
    big_coeff_24 = [r for r in progressive
                    if (coeff_buf_bytes(r["pil_w"], r["pil_h"], r["subsampling"]) or 0) > 24 * 1024 * 1024]
    print(f"  Progressive with >16 MiB coeff   : {len(big_coeff_16)}  "
          f"{[r['name'] for r in big_coeff_16]}")
    print(f"  Progressive with >24 MiB coeff   : {len(big_coeff_24)}  "
          f"{[r['name'] for r in big_coeff_24]}")

    if progressive:
        ranked = sorted(
            progressive,
            key=lambda r: coeff_buf_bytes(r["pil_w"], r["pil_h"], r["subsampling"]) or 0,
            reverse=True,
        )
        top_n = min(5, len(ranked))
        print(f"\n  Largest progressive coeff buffers (libjpeg-turbo DCT estimate):")
        for r in ranked[:top_n]:
            cb = coeff_buf_bytes(r["pil_w"], r["pil_h"], r["subsampling"]) or 0
            print(f"    {cb / (1024 * 1024):>6.2f} MiB  "
                  f"{r['pil_w']}x{r['pil_h']}  ss={r['subsampling']}  {r['name']}")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
