#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Self-test for host/p3a-flasher/flash_plan.py, and for its agreement with
what tools/make_release.py writes.

    python tools/test_flash_plan.py

Standard library only.
"""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "host" / "p3a-flasher"))
sys.path.insert(0, str(REPO / "tools"))
import flash_plan  # noqa: E402
import make_release  # noqa: E402
from flash_plan import PlanError  # noqa: E402
from test_make_release import app_image, build  # noqa: E402


class FlashPlan(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def folder(self, args: str, files: dict) -> str:
        (self.root / "flash_args").write_text(args, encoding="utf-8")
        for name, size in files.items():
            (self.root / name).write_bytes(bytes(size))
        return str(self.root)

    def test_pairs_are_read_in_order_and_options_are_passed_over(self):
        text = "--flash_mode dio --flash_size 32MB\r\n0x2000 bootloader.bin\r\n\r\n# note\n0X8000 partition-table.bin\n0x260000 p3a-v2.bin\n"
        self.assertEqual(flash_plan.parse(text),
                         [(0x2000, "bootloader.bin"), (0x8000, "partition-table.bin"), (0x260000, "p3a-v2.bin")])

    def test_a_path_is_reduced_to_its_file_name(self):
        self.assertEqual(flash_plan.parse("0x2000 bootloader/bootloader.bin\n0x20000 ..\\fw\\network_adapter.bin\n"),
                         [(0x2000, "bootloader.bin"), (0x20000, "network_adapter.bin")])

    def test_what_is_not_a_plan_is_refused(self):
        for text, why in (("", "nothing to write"), ("--flash_mode dio\n", "nothing to write"),
                          ("0x2000\n", "expected an address"), ("0x2000 a.bin extra\n", "expected an address"),
                          ("8192 a.bin\n", "not an address"), ("0xZZ a.bin\n", "not an address"),
                          ("0x2000000 a.bin\n", "not an address"), ("-0x10 a.bin\n", "not an address"),
                          ("0x2000 ..\n", "not a file name"), ("0x2000 a b.bin\n", "expected an address"),
                          ("0x2000 flash_args\n", "not a file name"), ("0x2000 .hidden\n", "not a file name"),
                          ("0x2000 a.bin\n0x3000 a.bin\n", "twice")):
            with self.assertRaisesRegex(PlanError, why, msg=repr(text)):
                flash_plan.parse(text)

    def test_a_folder_is_a_plan_when_every_file_is_there_and_none_runs_into_another(self):
        folder = self.folder("0x2000 a.bin\n0x3000 b.bin\n", {"a.bin": 0x1000, "b.bin": 16})
        plan = flash_plan.load(folder)
        self.assertEqual([(a, Path(p).name) for a, p in plan], [(0x2000, "a.bin"), (0x3000, "b.bin")])
        with self.assertRaisesRegex(PlanError, "a.bin would run into b.bin"):
            flash_plan.load(self.folder("0x2000 a.bin\n0x3000 b.bin\n", {"a.bin": 0x1001, "b.bin": 16}))
        with self.assertRaisesRegex(PlanError, "c.bin is missing"):
            flash_plan.load(self.folder("0x2000 a.bin\n0x9000 c.bin\n", {"a.bin": 16}))
        with self.assertRaisesRegex(PlanError, "past the end"):
            flash_plan.load(self.folder("0x1fffff0 a.bin\n", {"a.bin": 17}))
        with self.assertRaisesRegex(PlanError, "flash_args is missing"):
            flash_plan.load(str(self.root / "nowhere"))

    def test_a_release_that_blanks_the_settings_is_known_to_erase(self):
        self.assertTrue(flash_plan.erases(["bootloader.bin", "nvs-erased.bin"]))
        self.assertTrue(flash_plan.erases([str(self.root / "nvs-erased.bin")]))
        self.assertFalse(flash_plan.erases(["bootloader.bin", "p3a.bin", "storage.bin"]))
        self.assertEqual(flash_plan.ERASER, make_release.NVS_ERASED)

    def test_what_make_release_writes_is_a_plan_this_reads(self):
        out = make_release.make(build(self.root / "b", app=app_image("2.0.3")), self.root / "release")
        plan = flash_plan.load(str(out))
        self.assertEqual([(a, Path(p).name) for a, p in plan],
                         [(0x2000, "bootloader.bin"), (0x8000, "partition-table.bin"), (0x9000, "nvs-erased.bin"),
                          (0x1A000, "ota_data_initial.bin"), (0x20000, "network_adapter.bin"), (0x260000, "p3a-v2.bin")])
        self.assertTrue(flash_plan.erases(p for _, p in plan))

    def test_a_1x_release_is_a_plan_too(self):
        # v1.2.4's flash_args, as published.
        text = ("0x2000 bootloader.bin\n0x20000 p3a.bin\n0x8000 partition-table.bin\n0x1A000 ota_data_initial.bin\n"
                "0x1020000 storage.bin\n0x1420000 network_adapter.bin\n")
        pairs = flash_plan.parse(text)
        self.assertEqual(len(pairs), 6)
        self.assertFalse(flash_plan.erases(name for _, name in pairs))


if __name__ == "__main__":
    unittest.main()
