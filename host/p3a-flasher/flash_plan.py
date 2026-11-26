# SPDX-License-Identifier: Apache-2.0
"""What a flash writes: read from a release's `flash_args`.

A release folder (tools/make_release.py) carries a `flash_args` file, one
"address file" pair per line. The flasher holds no address and no list of
files of its own: 1.x kept both in two places and they had to be changed
together with the partition table.

No GUI and no esptool here, so that tools/test_flash_plan.py can test it.
"""
from __future__ import annotations

import os
import re

ARGS_NAME = "flash_args"
ERASER = "nvs-erased.bin"       # blank bytes over the settings: a release that has it erases the device
FLASH_BYTES = 32 * 1024 * 1024
_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}")


class PlanError(Exception):
    """The folder cannot be flashed as it is."""


def parse(text: str) -> list[tuple[int, str]]:
    """The (address, file name) pairs of a flash_args text, in the order given.

    Lines that start with "--" are esptool options (1.x wrote them there)
    and are passed over. A file is named plainly: no folder, nothing that
    could reach outside the release's folder.
    """
    pairs: list[tuple[int, str]] = []
    for number, raw in enumerate(text.replace("\r", "").split("\n"), 1):
        line = raw.strip()
        if not line or line.startswith("--") or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) != 2:
            raise PlanError(f"flash_args line {number}: expected an address and a file name")
        try:
            address = int(parts[0], 16) if parts[0].lower().startswith("0x") else None
        except ValueError:
            address = None
        if address is None or not 0 <= address < FLASH_BYTES:
            raise PlanError(f"flash_args line {number}: {parts[0]!r} is not an address in the flash")
        name = parts[1].replace("\\", "/").rsplit("/", 1)[-1]  # 1.x wrote build paths; the file's name is what counts
        if not _NAME.fullmatch(name) or name == ARGS_NAME:
            raise PlanError(f"flash_args line {number}: {parts[1]!r} is not a file name")
        if any(name == other for _, other in pairs):
            raise PlanError(f"flash_args names {name} twice")
        pairs.append((address, name))
    if not pairs:
        raise PlanError("flash_args lists nothing to write")
    return pairs


def load(folder: str) -> list[tuple[int, str]]:
    """The plan of a folder: (address, path) pairs, every file there, none over another."""
    path = os.path.join(folder, ARGS_NAME)
    if not os.path.isfile(path):
        raise PlanError(f"{ARGS_NAME} is missing")
    with open(path, encoding="utf-8", errors="replace") as f:
        pairs = parse(f.read())
    plan = []
    for address, name in pairs:
        file_path = os.path.join(folder, name)
        if not os.path.isfile(file_path):
            raise PlanError(f"{name} is missing")
        plan.append((address, file_path))
    spans = sorted((address, address + os.path.getsize(file_path), os.path.basename(file_path)) for address, file_path in plan)
    for (_, end, name), (start, _, next_name) in zip(spans, spans[1:]):
        if end > start:
            raise PlanError(f"{name} would run into {next_name}")
    if spans[-1][1] > FLASH_BYTES:
        raise PlanError(f"{spans[-1][2]} would run past the end of the flash")
    return plan


def erases(names) -> bool:
    """True if a flash of these file names leaves the device without its settings."""
    return any(os.path.basename(name) == ERASER for name in names)
