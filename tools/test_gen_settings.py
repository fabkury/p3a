#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Self-test for gen_settings.py: the real schema is accepted, and every kind
of bad entry is refused with a message that names it.

    python tools/test_gen_settings.py

Standard library only.
"""
from __future__ import annotations

import copy
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_settings  # noqa: E402

SCHEMA = Path(__file__).resolve().parent.parent / "components" / "p3a_settings" / "settings.json"

GOOD = [
    {"key": "level", "type": "int", "default": 5, "min": 0, "max": 10, "group": "g", "label": "Level"},
    {"key": "turn", "type": "int", "default": 0, "choices": [0, 90], "group": "g", "label": "Turn"},
    {"key": "on", "type": "bool", "default": True, "group": "g", "label": "On", "advanced": True},
    {"key": "mode", "type": "enum", "default": "b-2", "values": ["a", "b-2"], "group": "g", "label": "Mode"},
    {"key": "tint", "type": "color", "default": "#0a0B0c", "group": "g", "label": "Tint"},
    {"key": "name", "type": "string", "default": "x", "max_len": 8, "check": "device_name", "group": "g",
     "label": "Name \"quoted\"", "reboot": True},
    {"key": "api_key", "type": "string", "default": "", "max_len": 16, "group": "g", "label": "Key", "secret": True},
]


class RealSchema(unittest.TestCase):
    def setUp(self):
        self.settings = json.loads(SCHEMA.read_text(encoding="utf-8"))["settings"]

    def test_is_accepted(self):
        gen_settings.check_schema(self.settings)

    def test_keys_follow_the_naming_rule(self):
        for s in self.settings:
            key = s["key"]
            self.assertNotRegex(key, r"_(sec|secs|seconds|interval|msec)$", key)
            if s["type"] == "int" and re.search(r"refresh|dwell|duration", key):
                self.assertRegex(key, r"_(s|ms)$", f"{key}: a time needs a unit suffix")

    def test_every_api_key_is_secret(self):
        for s in self.settings:
            if s["key"].endswith("_api_key"):
                self.assertTrue(s.get("secret"), s["key"])

    def test_1x_names_are_not_reused_for_other_settings(self):
        keys = {s["key"] for s in self.settings}
        for s in self.settings:
            if "was" in s:
                self.assertNotIn(s["was"], keys - {s["key"]}, s["key"])


class Generation(unittest.TestCase):
    def test_output(self):
        gen_settings.check_schema(GOOD)
        header, source = gen_settings.generate(GOOD)
        self.assertIn("P3A_SET_LEVEL,\n    P3A_SET_TURN,", header)
        self.assertIn("P3A_SET_COUNT", header)
        self.assertIn("P3A_MODE_B_2 = 1,", header)
        self.assertIn("int32_t level;", header)
        self.assertIn("bool on;", header)
        self.assertIn("uint8_t mode;", header)
        self.assertIn("uint32_t tint;", header)
        self.assertIn("char name[9];", header)
        self.assertIn(".default_int = 5, .min = 0, .max = 10", source)
        self.assertIn("s_choices_turn[] = {0, 90}", source)
        self.assertIn('s_values_mode[] = {"a", "b-2"}', source)
        self.assertIn(".default_int = 1, .values = s_values_mode, .value_count = 2", source)
        self.assertIn(f".default_int = {0x0A0B0C}", source)
        self.assertIn('.label = "Name \\"quoted\\""', source)
        self.assertIn(".flags = P3A_SET_FLAG_REBOOT, .check = P3A_SET_CHECK_DEVICE_NAME", source)
        self.assertIn(".flags = P3A_SET_FLAG_ADVANCED", source)
        self.assertIn(".flags = P3A_SET_FLAG_SECRET", source)
        self.assertIn('.default_str = "x"', source)

    def test_main_writes_both_files_and_leaves_unchanged_files_alone(self):
        with tempfile.TemporaryDirectory() as tmp:
            schema = Path(tmp) / "s.json"
            out = Path(tmp) / "out"
            schema.write_text(json.dumps({"settings": GOOD}), encoding="utf-8")
            self.assertEqual(0, gen_settings.main(["gen", str(schema), str(out)]))
            header = out / "p3a_settings_gen.h"
            self.assertTrue(header.exists() and (out / "p3a_settings_gen.c").exists())
            stamp = header.stat().st_mtime_ns
            self.assertEqual(0, gen_settings.main(["gen", str(schema), str(out)]))
            self.assertEqual(stamp, header.stat().st_mtime_ns)

    def test_main_fails_on_a_bad_schema_and_on_bad_json(self):
        with tempfile.TemporaryDirectory() as tmp:
            schema = Path(tmp) / "s.json"
            bad = copy.deepcopy(GOOD)
            bad[0]["default"] = 11
            schema.write_text(json.dumps({"settings": bad}), encoding="utf-8")
            self.assertEqual(1, gen_settings.main(["gen", str(schema), tmp]))
            schema.write_text("{not json", encoding="utf-8")
            self.assertEqual(1, gen_settings.main(["gen", str(schema), tmp]))
            schema.write_text("{}", encoding="utf-8")
            self.assertEqual(1, gen_settings.main(["gen", str(schema), tmp]))


class Refusals(unittest.TestCase):
    def refuse(self, index: int, change: dict, expect: str, drop: tuple = ()):
        settings = copy.deepcopy(GOOD)
        settings[index].update(change)
        for field in drop:
            del settings[index][field]
        with self.assertRaises(gen_settings.SchemaError) as caught:
            gen_settings.check_schema(settings)
        self.assertIn(expect, str(caught.exception))

    def test_keys(self):
        self.refuse(0, {"key": "Level"}, "lower_snake_case")
        self.refuse(0, {"key": "a__b"}, "lower_snake_case")
        self.refuse(0, {"key": "x" * 32}, "lower_snake_case")
        self.refuse(0, {"key": "turn"}, "duplicate")
        self.refuse(0, {"colour": 1}, "unknown field")

    def test_common_fields(self):
        self.refuse(0, {"type": "float"}, "type must be")
        self.refuse(0, {}, "label is required", drop=("label",))
        self.refuse(0, {}, "group is required", drop=("group",))
        self.refuse(0, {}, "default is required", drop=("default",))
        self.refuse(0, {"reboot": "yes"}, "reboot must be")
        self.refuse(0, {"secret": True}, "only a string can be secret")

    def test_int(self):
        self.refuse(0, {"default": 11}, "outside 0..10")
        self.refuse(0, {"default": 1.5}, "must be an integer")
        self.refuse(0, {"default": True}, "must be an integer")
        self.refuse(0, {}, "needs min and max", drop=("max",))
        self.refuse(0, {"min": 11}, "ordered")
        self.refuse(0, {"max": 2**31}, "ordered")
        self.refuse(0, {"choices": [5]}, "not both")
        self.refuse(1, {"default": 45}, "not one of the choices")
        self.refuse(1, {"choices": [0, 0]}, "distinct")
        self.refuse(1, {"choices": []}, "distinct")
        self.refuse(0, {"values": ["a", "b"]}, "do not apply")

    def test_bool(self):
        self.refuse(2, {"default": 1}, "true or false")
        self.refuse(2, {"min": 0}, "do not apply")

    def test_enum(self):
        self.refuse(3, {"default": "c"}, "not one of the values")
        self.refuse(3, {"values": ["a"], "default": "a"}, "2 to 32")
        self.refuse(3, {"values": ["a", "a"], "default": "a"}, "2 to 32")
        self.refuse(3, {"values": ["a", "B"], "default": "a"}, "lower-case")

    def test_color(self):
        self.refuse(4, {"default": "#fff"}, "#1a2b3c")
        self.refuse(4, {"default": 0}, "#1a2b3c")

    def test_string(self):
        self.refuse(5, {"default": "123456789"}, "at most 8")
        self.refuse(5, {"default": "é"}, "ASCII")
        self.refuse(5, {"max_len": 0}, "1 to 255")
        self.refuse(5, {"max_len": 256}, "1 to 255")
        self.refuse(5, {"check": "postcode"}, "check must be")

    def test_size_limits(self):
        with self.assertRaises(gen_settings.SchemaError):
            gen_settings.check_schema([])
        many = [dict(GOOD[2], key=f"k{i}") for i in range(65)]
        with self.assertRaises(gen_settings.SchemaError) as caught:
            gen_settings.check_schema(many)
        self.assertIn("at most 64", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
