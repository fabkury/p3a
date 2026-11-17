#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Self-test for check_tree.py: every rule must fail on a tree that breaks it.

    python tools/test_check_tree.py

Builds small throwaway trees in a temporary directory. Standard library only.
"""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_tree  # noqa: E402

GUARD = "CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y\nCONFIG_ESP32P4_REV_MIN_1=y\n"
PARTITIONS = "ota_0, app, ota_0, 0x260000, 0x1000,\nota_1, app, ota_1, 0xC60000, 0x1000,\n"


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def component(root: Path, name: str, requires: str = "", source: str = "int x;\n", cmake_extra: str = "") -> None:
    req = f" REQUIRES {requires}" if requires else ""
    write(root / "components" / name / "CMakeLists.txt",
          f'idf_component_register(SRCS "{name}.c" INCLUDE_DIRS "include"{req}{cmake_extra})\n')
    write(root / "components" / name / f"{name}.c", source)


class Base(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        write(self.root / "sdkconfig.defaults", GUARD)
        write(self.root / "partitions.csv", PARTITIONS)
        write(self.root / "main" / "CMakeLists.txt", 'idf_component_register(SRCS "m.c" PRIV_REQUIRES low high)\n')
        write(self.root / "main" / "m.c", '#include "low.h"\nvoid app_main(void) {}\n')
        component(self.root, "low")
        component(self.root, "high", requires="low esp_timer")
        write(self.root / "components" / "LAYERS.txt", "# c layer\nlow util\nhigh net\n")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def problems(self) -> list[str]:
        return check_tree.check_sources(self.root)

    def assert_one(self, needle: str) -> None:
        found = self.problems()
        self.assertTrue(any(needle in p for p in found), f"expected a problem containing {needle!r}, got {found}")


class SourceChecks(Base):
    def test_clean_tree_passes(self) -> None:
        self.assertEqual(self.problems(), [])

    def test_unlisted_component(self) -> None:
        component(self.root, "stray")
        self.assert_one("components/stray is not listed")

    def test_listed_but_missing(self) -> None:
        write(self.root / "components" / "LAYERS.txt", "low util\nhigh net\nghost ui\n")
        self.assert_one("lists ghost")

    def test_unknown_layer(self) -> None:
        write(self.root / "components" / "LAYERS.txt", "low util\nhigh cloud\n")
        self.assert_one("unknown layer 'cloud'")

    def test_upward_requirement(self) -> None:
        component(self.root, "low", requires="high")
        component(self.root, "high")
        self.assert_one("which is a higher layer")

    def test_private_upward_requirement(self) -> None:
        write(self.root / "components" / "low" / "CMakeLists.txt",
              'idf_component_register(SRCS "low.c"\n    PRIV_REQUIRES high)\n')
        component(self.root, "high")
        self.assert_one("which is a higher layer")

    def test_same_layer_cycle(self) -> None:
        component(self.root, "a", requires="b")
        component(self.root, "b", requires="a")
        write(self.root / "components" / "LAYERS.txt", "low util\nhigh net\na storage\nb storage\n")
        self.assert_one("requirement cycle")

    def test_requires_main(self) -> None:
        component(self.root, "high", requires="low main")
        self.assert_one("requires main")

    def test_variable_in_requires(self) -> None:
        component(self.root, "high", requires="${deps}")
        self.assert_one("not a literal name")

    def test_commented_requires_is_ignored(self) -> None:
        write(self.root / "components" / "low" / "CMakeLists.txt",
              'idf_component_register(SRCS "low.c") # REQUIRES high\n')
        self.assertEqual(self.problems(), [])

    def test_weak_symbol(self) -> None:
        component(self.root, "low", source="void f(void) __attribute__((weak));\n")
        self.assert_one("__attribute__((weak))")

    def test_weak_symbol_spaced(self) -> None:
        component(self.root, "low", source="__attribute__ (( __weak__ )) void f(void);\n")
        self.assert_one("__attribute__((weak))")

    def test_whole_archive(self) -> None:
        component(self.root, "low", cmake_extra=" WHOLE_ARCHIVE")
        self.assert_one("WHOLE_ARCHIVE")

    def test_include_from_main(self) -> None:
        component(self.root, "low", source='#include "../../main/include/app.h"\n')
        self.assert_one("from main/")

    def test_include_from_legacy(self) -> None:
        component(self.root, "low", source='#include "../../legacy/main/include/app_lcd.h"\n')
        self.assert_one("from legacy/")

    def test_cmake_refers_to_legacy(self) -> None:
        component(self.root, "low", cmake_extra=' PRIV_INCLUDE_DIRS "../../legacy/main/include"')
        self.assert_one("refers to legacy/")

    def test_main_may_not_include_legacy(self) -> None:
        write(self.root / "main" / "m.c", '#include "legacy/x.h"\n')
        self.assert_one("from legacy/")

    def test_vendored_code_is_not_scanned_but_is_layered(self) -> None:
        component(self.root, "blob", source="void f(void) __attribute__((weak));\n")
        write(self.root / "components" / "LAYERS.txt", "low util\nhigh net\nblob player vendored\n")
        self.assertEqual(self.problems(), [])

    def test_missing_guard(self) -> None:
        write(self.root / "sdkconfig.defaults", "CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y\n")
        self.assert_one("CONFIG_ESP32P4_REV_MIN_1=y")

    def test_commented_guard_does_not_count(self) -> None:
        write(self.root / "sdkconfig.defaults", "CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y\n# CONFIG_ESP32P4_REV_MIN_1=y\n")
        self.assert_one("CONFIG_ESP32P4_REV_MIN_1=y")


class BuildChecks(Base):
    def make_build(self, cfg: str = "CONFIG_ESP32P4_REV_MIN_FULL=1\n", size: int = 100, map_text: str = "") -> Path:
        build = self.root / "build"
        write(build / "sdkconfig", cfg)
        (build / "p3a.bin").write_bytes(b"\0" * size)
        write(build / "p3a.map", map_text)
        return build

    def build_problems(self, build: Path, test_build: bool = False) -> list[str]:
        return check_tree.check_build(self.root, build, test_build)

    def test_clean_build_passes(self) -> None:
        self.assertEqual(self.build_problems(self.make_build()), [])

    def test_wrong_revision(self) -> None:
        found = self.build_problems(self.make_build(cfg="CONFIG_ESP32P4_REV_MIN_FULL=301\n"))
        self.assertTrue(any("REV_MIN_FULL" in p for p in found), found)

    def test_image_too_large(self) -> None:
        found = self.build_problems(self.make_build(size=0x1000 * 91 // 100))
        self.assertTrue(any("above 90%" in p for p in found), found)

    def test_image_at_limit_passes(self) -> None:
        self.assertEqual(self.build_problems(self.make_build(size=int(0x1000 * 0.90))), [])

    def test_hooks_in_release(self) -> None:
        cfg = "CONFIG_ESP32P4_REV_MIN_FULL=1\nCONFIG_P3A_TEST_HOOKS=y\n"
        found = self.build_problems(self.make_build(cfg=cfg))
        self.assertTrue(any("release build has CONFIG_P3A_TEST_HOOKS" in p for p in found), found)

    def test_hook_symbols_in_release(self) -> None:
        found = self.build_problems(self.make_build(map_text=" .text.p3a_test_console_start\n"))
        self.assertTrue(any("test-hook symbols" in p for p in found), found)

    def test_test_build_needs_hooks(self) -> None:
        found = self.build_problems(self.make_build(), test_build=True)
        self.assertTrue(any("without CONFIG_P3A_TEST_HOOKS" in p for p in found), found)

    def test_test_build_with_hooks_passes(self) -> None:
        cfg = "CONFIG_ESP32P4_REV_MIN_FULL=1\nCONFIG_P3A_TEST_HOOKS=y\n"
        build = self.make_build(cfg=cfg, map_text=" .text.p3a_test_console_start\n")
        self.assertEqual(self.build_problems(build, test_build=True), [])

    def test_missing_image(self) -> None:
        build = self.make_build()
        (build / "p3a.bin").unlink()
        found = self.build_problems(build)
        self.assertTrue(any("p3a.bin is missing" in p for p in found), found)


if __name__ == "__main__":
    unittest.main()
