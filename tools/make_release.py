#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Makes the folder of files that a p3a 2.x release is published with.

    python host/p3a-lab/lab.py build release
    python tools/make_release.py                 # reads build/, writes release/v<version>/

What it writes, all to be uploaded as the release's assets:

    p3a-v2.bin             the firmware. Devices update themselves from this
                           file and from no other (docs/2.0/DECISIONS.md D85):
                           the name must not change within 2.x, and no other
                           series may use it.
    bootloader.bin, partition-table.bin, ota_data_initial.bin,
    network_adapter.bin    the rest of what a USB flash writes
    nvs-erased.bin         blank bytes written over the settings partition, so
                           that a USB flash leaves a fresh device whatever ran
                           on it before (docs/2.0/DECISIONS.md C6, D89): a 1.x
                           unit's settings mean nothing to 2.x
    flash_args             address and file name, one pair per line: what the
                           web flasher and the flasher program read
    flash_command.txt      the esptool command, for doing it by hand
    SHA256SUMS             for people; devices use the digest GitHub computes
    README.md              what the files are
    p3a-<version>-usb-flash.zip
                           the files a USB flash needs, in one download: for
                           esptool by hand and for the flasher program's
                           "local ZIP"

What it refuses to package, because it must not ship:

    a build with the test hooks or the frame trace compiled in
    a version that ends in "-dev" (unless --allow-dev, for a rehearsal)
    an image that is not whole, is not p3a, or does not say the version
    an image larger than 90% of its slot (docs/2.0-architecture-review/
        target-architecture.md, section 4: room for the next ones)
    a build whose flash layout is not the 2.x partition table

Standard library only. tools/test_make_release.py is its self-test.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

ASSET_NAME = "p3a-v2.bin"   # update_release.h: UPDATE_ASSET_NAME
PROJECT_NAME = "p3a"
SLOT_SHARE = 0.90
DESC_AT = 32                # the app description: after the image header and the first segment's header
DESC_MAGIC = bytes.fromhex("3254cdab")
# What a USB flash writes, as flasher_args.json names each part, with the name it ships under.
PARTS = (
    ("bootloader", "bootloader.bin"),
    ("partition-table", "partition-table.bin"),
    ("otadata", "ota_data_initial.bin"),
    ("slave_fw", "network_adapter.bin"),
    ("app", ASSET_NAME),
)
# Where the 2.x partition table puts them (partitions.csv; fixed for the life of 2.x, D1).
LAYOUT = {"otadata": 0x1A000, "slave_fw": 0x20000, "app": 0x260000}
NVS_ERASED = "nvs-erased.bin"


class Refused(Exception):
    """The build must not be released as it is."""


def read_app(image: bytes) -> dict:
    """Checks that `image` is a whole app image and returns what it says of itself."""
    if len(image) < DESC_AT + 256 or image[0] != 0xE9:
        raise Refused("the app is not a firmware image")
    if image[23] != 1:
        raise Refused("the app image has no SHA-256 appended")
    if hashlib.sha256(image[:-32]).digest() != image[-32:]:
        raise Refused("the app image is damaged: its appended SHA-256 does not match")
    desc = image[DESC_AT:DESC_AT + 256]
    if desc[:4] != DESC_MAGIC:
        raise Refused("the app image has no app description where one is expected")

    def text(at: int) -> str:
        return desc[at:at + 32].split(b"\0", 1)[0].decode("ascii", "replace")

    return {"version": text(16), "project": text(48), "idf": text(112)}


def read_partitions(table: bytes) -> dict:
    """The partition table's entries by label: (type, subtype, offset, size)."""
    out = {}
    for at in range(0, len(table) - 31, 32):
        entry = table[at:at + 32]
        if entry[:2] != b"\xaa\x50":
            break
        kind, subtype, offset, size = struct.unpack("<BBII", entry[2:12])
        out[entry[12:28].split(b"\0", 1)[0].decode()] = (kind, subtype, offset, size)
    return out


def make(build: Path, out_root: Path, allow_dev: bool = False) -> Path:
    args_path = build / "flasher_args.json"
    if not args_path.is_file():
        raise Refused(f"{args_path} is missing: build first (python host/p3a-lab/lab.py build release)")
    args = json.loads(args_path.read_text(encoding="utf-8"))
    config = json.loads((build / "config" / "sdkconfig.json").read_text(encoding="utf-8"))

    for option, what in (("P3A_TEST_HOOKS", "the test hooks"), ("P3A_FRAME_TRACE", "the frame trace")):
        if config.get(option):
            raise Refused(f"this build has {what} compiled in: release from the release build")
    if config.get("IDF_TARGET") != "esp32p4":
        raise Refused(f"this build is for {config.get('IDF_TARGET')!r}, not esp32p4")

    files = {}
    for part, name in PARTS:
        if part not in args:
            raise Refused(f"flasher_args.json names no {part}")
        source = (build / args[part]["file"]).resolve()
        if not source.is_file():
            raise Refused(f"{source} is missing")
        files[part] = (int(args[part]["offset"], 16), name, source.read_bytes())

    partitions = read_partitions(files["partition-table"][2])
    for label in ("nvs", "otadata", "slave_fw", "ota_0", "ota_1"):
        if label not in partitions:
            raise Refused(f"the partition table has no {label}: this is not the 2.x layout")
    for part, label in (("otadata", "otadata"), ("slave_fw", "slave_fw"), ("app", "ota_0")):
        offset = partitions[label][2]
        if files[part][0] != offset or LAYOUT[part] != offset:
            raise Refused(f"{part} would be written at {files[part][0]:#x}; the 2.x layout has {label} at {LAYOUT[part]:#x}")
    if partitions["ota_0"][3] != partitions["ota_1"][3]:
        raise Refused("the two app slots differ in size")
    if len(files["slave_fw"][2]) > partitions["slave_fw"][3]:
        raise Refused("the co-processor image does not fit its partition")

    app = files["app"][2]
    said = read_app(app)
    if said["project"] != PROJECT_NAME:
        raise Refused(f"the app says it is {said['project']!r}, not {PROJECT_NAME!r}")
    version = said["version"]
    parts = version.split("-", 1)
    numbers = parts[0].split(".")
    if len(numbers) != 3 or not all(n.isdigit() for n in numbers) or numbers[0] != "2":
        raise Refused(f"{version!r} is not a 2.x version: {ASSET_NAME} is for 2.x firmware only")
    if version.endswith("-dev") and not allow_dev:
        raise Refused(f"the version is {version}: set PROJECT_VER to the release's version first "
                      "(or pass --allow-dev for a rehearsal)")
    slot = partitions["ota_0"][3]
    limit = int(slot * SLOT_SHARE)
    if len(app) > limit:
        raise Refused(f"the app is {len(app)} bytes; the limit is {limit} ({SLOT_SHARE:.0%} of the {slot}-byte slot)")

    out = out_root / f"v{version}"
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    # The settings partition, blank: an erased flash reads as 0xFF.
    nvs_offset, nvs_size = partitions["nvs"][2], partitions["nvs"][3]
    files["nvs"] = (nvs_offset, NVS_ERASED, b"\xff" * nvs_size)
    ordered = sorted(files.values())
    for _, name, data in ordered:
        (out / name).write_bytes(data)

    settings = args.get("flash_settings", {})
    # The spellings with underscores: esptool 4 (the one an IDF 5.5 install has) takes only
    # those, and esptool 5 still takes them.
    options = (f"--flash_mode {settings.get('flash_mode', 'dio')} --flash_freq {settings.get('flash_freq', '80m')} "
               f"--flash_size {settings.get('flash_size', '32MB')}")
    write(out / "flash_args", "".join(f"{offset:#x} {name}\n" for offset, name, _ in ordered))
    # --force: network_adapter.bin is an ESP32-C6 image that sits in the ESP32-P4's flash by design.
    command = f"python -m esptool --chip esp32p4 -b 460800 --before default_reset --after hard_reset write_flash {options} --force"
    write(out / "flash_command.txt",
          "# Run from this folder. Add -p <port> if the port is not found by itself.\n"
          "# This writes the whole device: its settings, Wi-Fi network and registrations are erased.\n\n"
          f"{command} @flash_args\n\n"
          "# In PowerShell, quote the last argument:\n"
          f"{command} \"@flash_args\"\n")
    write(out / "SHA256SUMS", "".join(f"{hashlib.sha256(data).hexdigest()}  {name}\n" for _, name, data in ordered))
    write(out / "README.md", readme(version, ordered, len(app), slot))
    # One download for a flash by hand. Fixed dates, so that the same build gives the same zip.
    bundle = out / f"p3a-{version}-usb-flash.zip"
    with zipfile.ZipFile(bundle, "w", zipfile.ZIP_DEFLATED) as z:
        for name in [name for _, name, _ in ordered] + ["flash_args", "flash_command.txt", "README.md"]:
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, (out / name).read_bytes())
    print(f"release {version}: {out}")
    print(f"  {ASSET_NAME}: {len(app)} bytes, {len(app) / slot:.1%} of its slot, built with IDF {said['idf']}")
    print(f"  sha256 {hashlib.sha256(app).hexdigest()}")
    print("  upload every file in the folder as the release's assets")
    return out


def write(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8", newline="\n")


def readme(version: str, files: list, app_bytes: int, slot: int) -> str:
    rows = "".join(f"| `{name}` | {offset:#x} | {len(data):,} |\n" for offset, name, data in files)
    return f"""# p3a {version}

Firmware for the p3a pixel art player (Waveshare ESP32-P4-WIFI6-Touch-LCD-4B).

## A device that already runs p3a 2.x

Nothing to download: open the device's page, go to **Update**, and install.
The device takes `{ASSET_NAME}` from this release by itself.

## A new device, or one that runs p3a 1.x

Flash it over USB, with the web flasher (https://fabkury.github.io/p3a/web-flasher/)
or with `p3a-flasher.exe` if this release has one.

**A USB flash erases the device**: its settings, its Wi-Fi network and its
Makapix Club registration (`{NVS_ERASED}` is written over them). Files on
the SD card are kept. A 1.x device cannot update to 2.x over the air, and
nothing of its settings carries over.

By hand: download `p3a-{version}-usb-flash.zip`, extract it, and run the
esptool command in its `flash_command.txt`.

## The files

| File | Written at | Bytes |
|---|---|---|
{rows}
`{ASSET_NAME}` uses {app_bytes / slot:.1%} of its slot. `SHA256SUMS` lists each
file's SHA-256 (`sha256sum -c SHA256SUMS`).
"""


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--build", type=Path, default=REPO / "build", help="the release build's folder (default: build)")
    parser.add_argument("--out", type=Path, default=REPO / "release", help="where v<version>/ is made (default: release)")
    parser.add_argument("--allow-dev", action="store_true", help="package a -dev version, for a rehearsal")
    options = parser.parse_args(argv)
    try:
        make(options.build, options.out, options.allow_dev)
    except Refused as why:
        print(f"make_release: refused: {why}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
