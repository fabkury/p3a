#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Self-test for make_release.py.

    python tools/test_make_release.py

Makes small stand-ins for a build's outputs and checks what is packaged
and what is refused. Standard library only.
"""
from __future__ import annotations

import hashlib
import json
import re
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_release  # noqa: E402
from make_release import Refused  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
SLOT = 0xA00000


def app_image(version: str = "2.0.0", project: str = "p3a", size: int = 4096) -> bytes:
    """A stand-in with what make_release reads of an app: header, description, appended SHA-256."""
    header = bytearray(24)
    header[0] = 0xE9
    header[23] = 1
    desc = bytearray(256)
    desc[0:4] = bytes.fromhex("3254cdab")
    desc[16:16 + len(version)] = version.encode()
    desc[48:48 + len(project)] = project.encode()
    desc[112:118] = b"v5.5.4"
    body = bytes(header) + bytes(8) + bytes(desc)
    body += bytes(size - 32 - len(body))
    return body + hashlib.sha256(body).digest()


def table(entries=None) -> bytes:
    entries = entries or [("nvs", 1, 2, 0x9000, 0x4000), ("otadata", 1, 0, 0x1A000, 0x2000),
                          ("slave_fw", 1, 0x40, 0x20000, 0x200000), ("ota_0", 0, 0x10, 0x260000, SLOT),
                          ("ota_1", 0, 0x11, 0xC60000, SLOT)]
    out = b""
    for label, kind, subtype, offset, size in entries:
        out += b"\xaa\x50" + struct.pack("<BBII", kind, subtype, offset, size) + label.encode().ljust(16, b"\0") + bytes(4)
    return out + b"\xeb\xeb" + bytes(30)


def build(root: Path, app: bytes | None = None, config: dict | None = None, offsets: dict | None = None,
          partitions: bytes | None = None, slave: bytes = b"c6" * 500) -> Path:
    b = root / "build"
    (b / "bootloader").mkdir(parents=True)
    (b / "partition_table").mkdir()
    (b / "config").mkdir()
    (root / "fw").mkdir()
    (b / "bootloader" / "bootloader.bin").write_bytes(b"boot" * 100)
    (b / "partition_table" / "partition-table.bin").write_bytes(partitions or table())
    (b / "ota_data_initial.bin").write_bytes(b"\xff" * 0x2000)
    (b / "p3a.bin").write_bytes(app if app is not None else app_image())
    (root / "fw" / "network_adapter.bin").write_bytes(slave)
    at = {"bootloader": "0x2000", "partition-table": "0x8000", "otadata": "0x1a000", "slave_fw": "0x20000", "app": "0x260000"}
    at.update(offsets or {})
    files = {"bootloader": "bootloader/bootloader.bin", "partition-table": "partition_table/partition-table.bin",
             "otadata": "ota_data_initial.bin", "slave_fw": "../fw/network_adapter.bin", "app": "p3a.bin"}
    args = {"flash_settings": {"flash_mode": "dio", "flash_size": "32MB", "flash_freq": "80m"}}
    for part, file in files.items():
        args[part] = {"offset": at[part], "file": file}
    (b / "flasher_args.json").write_text(json.dumps(args), encoding="utf-8")
    cfg = {"IDF_TARGET": "esp32p4", "P3A_TEST_HOOKS": False, "P3A_FRAME_TRACE": False}
    cfg.update(config or {})
    (b / "config" / "sdkconfig.json").write_text(json.dumps(cfg), encoding="utf-8")
    return b


class MakeRelease(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def refused(self, why: str, **kw) -> None:
        allow_dev = kw.pop("allow_dev", False)
        with self.assertRaisesRegex(Refused, why):
            make_release.make(build(self.root, **kw), self.root / "release", allow_dev)
        self.assertFalse((self.root / "release").exists(), "nothing is written when a build is refused")

    def test_a_release_build_is_packaged_under_the_names_devices_and_flashers_expect(self):
        app = app_image("2.0.3")
        out = make_release.make(build(self.root, app=app), self.root / "release")
        self.assertEqual(out, self.root / "release" / "v2.0.3")
        self.assertEqual(sorted(p.name for p in out.iterdir()),
                         ["README.md", "SHA256SUMS", "bootloader.bin", "flash_args", "flash_command.txt",
                          "network_adapter.bin", "nvs-erased.bin", "ota_data_initial.bin", "p3a-v2.bin",
                          "partition-table.bin"])
        # The settings partition is written blank, all of it: a USB flash leaves a fresh device.
        self.assertEqual((out / "nvs-erased.bin").read_bytes(), b"\xff" * 0x4000)
        self.assertEqual((out / "p3a-v2.bin").read_bytes(), app, "the firmware, byte for byte, under the 2.x name")
        self.assertEqual((out / "flash_args").read_text(),
                         "0x2000 bootloader.bin\n0x8000 partition-table.bin\n0x9000 nvs-erased.bin\n0x1a000 ota_data_initial.bin\n"
                         "0x20000 network_adapter.bin\n0x260000 p3a-v2.bin\n")
        sums = dict(reversed(line.split("  ")) for line in (out / "SHA256SUMS").read_text().splitlines())
        self.assertEqual(sums["p3a-v2.bin"], hashlib.sha256(app).hexdigest())
        self.assertEqual(len(sums), 6)
        command = (out / "flash_command.txt").read_text()
        self.assertIn("--chip esp32p4", command)
        self.assertIn("--flash_size 32MB", command)
        self.assertIn("--before default_reset --after hard_reset write_flash", command, "as esptool 4 spells them")
        self.assertIn("--force @flash_args", command)
        readme = (out / "README.md").read_text(encoding="utf-8")
        self.assertIn("# p3a 2.0.3", readme)
        self.assertIn("erases the device", readme)
        self.assertNotIn("storage.bin", readme)
        self.assertNotIn("—", readme)

    def test_the_asset_name_is_the_one_the_firmware_looks_for(self):
        header = (REPO / "components" / "p3a_update" / "update_release.h").read_text(encoding="utf-8")
        self.assertEqual(re.search(r'#define UPDATE_ASSET_NAME\s+"([^"]+)"', header).group(1), make_release.ASSET_NAME)
        # And the layout is the partition table's.
        rows = {}
        for line in (REPO / "partitions.csv").read_text(encoding="utf-8").splitlines():
            cells = [c.strip() for c in line.split(",")]
            if len(cells) >= 5 and not line.startswith("#"):
                rows[cells[0]] = int(cells[3], 16)
        self.assertEqual(make_release.LAYOUT, {"otadata": rows["otadata"], "slave_fw": rows["slave_fw"], "app": rows["ota_0"]})

    def test_packaging_again_replaces_the_folder(self):
        b = build(self.root)
        out = make_release.make(b, self.root / "release")
        (out / "stale.txt").write_text("left over")
        make_release.make(b, self.root / "release")
        self.assertFalse((out / "stale.txt").exists())

    def test_a_build_with_test_code_is_refused(self):
        self.refused("test hooks", config={"P3A_TEST_HOOKS": True})

    def test_a_build_that_traces_is_refused(self):
        self.refused("frame trace", config={"P3A_FRAME_TRACE": True})

    def test_another_chip_is_refused(self):
        self.refused("not esp32p4", config={"IDF_TARGET": "esp32s3"})

    def test_a_dev_version_is_refused_unless_it_is_a_rehearsal(self):
        self.refused("2.0.0-dev", app=app_image("2.0.0-dev"))
        out = make_release.make(build(self.root / "again", app=app_image("2.0.0-dev")), self.root / "release", allow_dev=True)
        self.assertEqual(out.name, "v2.0.0-dev")

    def test_a_prerelease_version_is_packaged(self):
        out = make_release.make(build(self.root, app=app_image("2.1.0-rc.1")), self.root / "release")
        self.assertEqual(out.name, "v2.1.0-rc.1")

    def test_a_version_of_another_series_is_refused(self):
        self.refused("not a 2.x version", app=app_image("1.2.5"))

    def test_a_version_that_is_not_one_is_refused(self):
        self.refused("not a 2.x version", app=app_image("two"))

    def test_another_project_is_refused(self):
        self.refused("not 'p3a'", app=app_image(project="blink"))

    def test_a_damaged_image_is_refused(self):
        image = bytearray(app_image())
        image[1000] ^= 1
        self.refused("damaged", app=bytes(image))

    def test_what_is_not_an_image_is_refused(self):
        self.refused("not a firmware image", app=b"<html>" + bytes(5000))

    def test_an_image_over_nine_tenths_of_its_slot_is_refused(self):
        limit = int(SLOT * 0.9)
        self.refused("the limit is", app=app_image(size=limit + 16))
        out = make_release.make(build(self.root / "fits", app=app_image(size=limit)), self.root / "release")
        self.assertEqual((out / "p3a-v2.bin").stat().st_size, limit)

    def test_a_layout_that_is_not_the_2x_partition_table_is_refused(self):
        self.refused("the 2.x layout has ota_0 at 0x260000", offsets={"app": "0x20000"})

    def test_a_table_without_the_2x_partitions_is_refused(self):
        self.refused("no slave_fw", partitions=table([("nvs", 1, 2, 0x9000, 0x4000), ("otadata", 1, 0, 0x1A000, 0x2000),
                                                      ("ota_0", 0, 0x10, 0x260000, SLOT), ("ota_1", 0, 0x11, 0xC60000, SLOT)]))

    def test_a_co_processor_image_too_large_for_its_partition_is_refused(self):
        self.refused("does not fit", slave=bytes(0x200001))

    def test_a_folder_that_is_not_a_build_is_refused(self):
        with self.assertRaisesRegex(Refused, "build first"):
            make_release.make(self.root / "nothing", self.root / "release")


if __name__ == "__main__":
    unittest.main()
