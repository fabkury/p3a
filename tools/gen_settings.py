#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generates the firmware's settings table from components/p3a_settings/settings.json.

    python tools/gen_settings.py <settings.json> <output-dir>

Writes p3a_settings_gen.h (the setting ids, the enum constants and the typed
struct p3a_settings_t) and p3a_settings_gen.c (the table the store validates
against). Both the firmware build and the host tests run this at configure
time; nothing generated is checked in.

The schema is checked first, and a bad entry stops the build with a message
that names it. Standard library only.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

TYPES = ("bool", "int", "string", "enum", "color")
CHECKS = ("device_name", "sd_root", "country")
ALLOWED = {
    "key", "type", "default", "min", "max", "choices", "max_len", "values", "check",
    "group", "label", "reboot", "advanced", "secret", "was",
}
KEY_RE = re.compile(r"^[a-z][a-z0-9]*(_[a-z0-9]+)*$")
COLOR_RE = re.compile(r"^#[0-9a-fA-F]{6}$")
MAX_SETTINGS = 64     # the store reports changes as a 64-bit mask
MAX_KEY_LEN = 31
INT32 = (-2**31, 2**31 - 1)


class SchemaError(Exception):
    pass


def check_schema(settings: list[dict]) -> None:
    if not settings:
        raise SchemaError("no settings")
    if len(settings) > MAX_SETTINGS:
        raise SchemaError(f"{len(settings)} settings; the store supports at most {MAX_SETTINGS}")
    seen: set[str] = set()
    for i, s in enumerate(settings):
        key = s.get("key")
        where = f"setting #{i + 1} ({key!r})"
        if not isinstance(key, str) or not KEY_RE.match(key) or len(key) > MAX_KEY_LEN:
            raise SchemaError(f"{where}: key must be lower_snake_case, at most {MAX_KEY_LEN} characters")
        if key in seen:
            raise SchemaError(f"{where}: duplicate key")
        seen.add(key)
        unknown = set(s) - ALLOWED
        if unknown:
            raise SchemaError(f"{where}: unknown field(s) {sorted(unknown)}")
        t = s.get("type")
        if t not in TYPES:
            raise SchemaError(f"{where}: type must be one of {TYPES}")
        for field in ("group", "label"):
            if not isinstance(s.get(field), str) or not s[field]:
                raise SchemaError(f"{where}: {field} is required")
        for field in ("reboot", "advanced", "secret"):
            if field in s and not isinstance(s[field], bool):
                raise SchemaError(f"{where}: {field} must be true or false")
        if "default" not in s:
            raise SchemaError(f"{where}: default is required")
        d = s["default"]

        def only(*fields):
            extra = {"min", "max", "choices", "max_len", "values", "check"} & set(s) - set(fields)
            if extra:
                raise SchemaError(f"{where}: {sorted(extra)} do not apply to type {t}")

        if t == "bool":
            only()
            if not isinstance(d, bool):
                raise SchemaError(f"{where}: default must be true or false")
        elif t == "int":
            only("min", "max", "choices")
            if isinstance(d, bool) or not isinstance(d, int):
                raise SchemaError(f"{where}: default must be an integer")
            if "choices" in s:
                c = s["choices"]
                if "min" in s or "max" in s:
                    raise SchemaError(f"{where}: give choices or min/max, not both")
                if (not isinstance(c, list) or not c or len(c) > 16 or len(set(c)) != len(c)
                        or any(isinstance(v, bool) or not isinstance(v, int) for v in c)):
                    raise SchemaError(f"{where}: choices must be 1 to 16 distinct integers")
                if d not in c:
                    raise SchemaError(f"{where}: default {d} is not one of the choices")
            else:
                lo, hi = s.get("min"), s.get("max")
                if any(isinstance(v, bool) or not isinstance(v, int) for v in (lo, hi)):
                    raise SchemaError(f"{where}: an int needs min and max, or choices")
                if not (INT32[0] <= lo <= hi <= INT32[1]):
                    raise SchemaError(f"{where}: min and max must be ordered 32-bit values")
                if not lo <= d <= hi:
                    raise SchemaError(f"{where}: default {d} is outside {lo}..{hi}")
        elif t == "string":
            only("max_len", "check")
            n = s.get("max_len")
            if isinstance(n, bool) or not isinstance(n, int) or not 1 <= n <= 255:
                raise SchemaError(f"{where}: max_len must be 1 to 255")
            if not isinstance(d, str) or len(d) > n or not d.isascii():
                raise SchemaError(f"{where}: default must be an ASCII string of at most {n} characters")
            if "check" in s and s["check"] not in CHECKS:
                raise SchemaError(f"{where}: check must be one of {CHECKS}")
        elif t == "enum":
            only("values")
            v = s.get("values")
            if (not isinstance(v, list) or not 2 <= len(v) <= 32 or len(set(v)) != len(v)
                    or any(not isinstance(x, str) or not re.match(r"^[a-z0-9][a-z0-9_-]*$", x) for x in v)):
                raise SchemaError(f"{where}: values must be 2 to 32 distinct lower-case names")
            if d not in v:
                raise SchemaError(f"{where}: default {d!r} is not one of the values")
        elif t == "color":
            only()
            if not isinstance(d, str) or not COLOR_RE.match(d):
                raise SchemaError(f"{where}: default must look like \"#1a2b3c\"")
        if s.get("secret") and t != "string":
            raise SchemaError(f"{where}: only a string can be secret")


def c_string(text: str) -> str:
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def ident(text: str) -> str:
    return re.sub(r"[^A-Za-z0-9]", "_", text).upper()


def field_decl(s: dict) -> str:
    t, key = s["type"], s["key"]
    if t == "bool":
        return f"    bool {key};"
    if t == "int":
        return f"    int32_t {key};"
    if t == "enum":
        return f"    uint8_t {key};  // P3A_{ident(key)}_*"
    if t == "color":
        return f"    uint32_t {key};  // 0xRRGGBB"
    return f"    char {key}[{s['max_len'] + 1}];"


def default_int(s: dict) -> int:
    t, d = s["type"], s["default"]
    if t == "bool":
        return int(d)
    if t == "int":
        return d
    if t == "enum":
        return s["values"].index(d)
    if t == "color":
        return int(d[1:], 16)
    return 0


def generate(settings: list[dict]) -> tuple[str, str]:
    banner = "// GENERATED by tools/gen_settings.py from components/p3a_settings/settings.json. Do not edit.\n"

    h = [banner, "#pragma once", "", "#include <stdbool.h>", "#include <stdint.h>", "",
         "// One id per setting, in schema order.", "typedef enum {"]
    h += [f"    P3A_SET_{ident(s['key'])}," for s in settings]
    h += ["    P3A_SET_COUNT", "} p3a_setting_id_t;", ""]
    for s in settings:
        if s["type"] == "enum":
            h.append(f"// {s['key']}")
            h.append("enum {")
            h += [f"    P3A_{ident(s['key'])}_{ident(v)} = {i}," for i, v in enumerate(s["values"])]
            h += ["};", ""]
    h += ["// Every setting, typed. Filled by p3a_settings_get().", "typedef struct {"]
    h += [field_decl(s) for s in settings]
    h += ["} p3a_settings_t;", ""]

    c = [banner, '#include "p3a_settings_gen.h"', "", "#include <stddef.h>", "", '#include "p3a_settings_schema.h"', ""]
    for s in settings:
        if s["type"] == "enum":
            vals = ", ".join(c_string(v) for v in s["values"])
            c.append(f"static const char *const s_values_{s['key']}[] = {{{vals}}};")
        if s["type"] == "int" and "choices" in s:
            c.append(f"static const int32_t s_choices_{s['key']}[] = {{{', '.join(str(v) for v in s['choices'])}}};")
    c += ["", "const p3a_setting_def_t p3a_setting_defs[P3A_SET_COUNT] = {"]
    for s in settings:
        key, t = s["key"], s["type"]
        flags = [f for f, on in (("P3A_SET_FLAG_REBOOT", s.get("reboot")), ("P3A_SET_FLAG_ADVANCED", s.get("advanced")),
                                 ("P3A_SET_FLAG_SECRET", s.get("secret"))) if on]
        fields = [
            f".key = {c_string(key)}",
            f".group = {c_string(s['group'])}",
            f".label = {c_string(s['label'])}",
            f".type = P3A_SET_TYPE_{t.upper()}",
            f".flags = {' | '.join(flags) if flags else '0'}",
            f".check = P3A_SET_CHECK_{s.get('check', 'none').upper()}",
            f".offset = offsetof(p3a_settings_t, {key})",
            f".size = sizeof(((p3a_settings_t *)0)->{key})",
            f".default_int = {default_int(s)}",
        ]
        if t == "int" and "choices" not in s:
            fields += [f".min = {s['min']}", f".max = {s['max']}"]
        if t == "int" and "choices" in s:
            fields += [f".choices = s_choices_{key}", f".choice_count = {len(s['choices'])}"]
        if t == "enum":
            fields += [f".values = s_values_{key}", f".value_count = {len(s['values'])}"]
        if t == "string":
            fields.append(f".default_str = {c_string(s['default'])}")
        c.append(f"    [P3A_SET_{ident(key)}] = {{{', '.join(fields)}}},")
    c += ["};", ""]
    return "\n".join(h), "\n".join(c)


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__)
        return 2
    schema_path, out_dir = Path(argv[1]), Path(argv[2])
    try:
        settings = json.loads(schema_path.read_text(encoding="utf-8"))["settings"]
        check_schema(settings)
    except (SchemaError, KeyError, json.JSONDecodeError) as exc:
        print(f"{schema_path}: {exc}", file=sys.stderr)
        return 1
    header, source = generate(settings)
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, text in (("p3a_settings_gen.h", header), ("p3a_settings_gen.c", source)):
        path = out_dir / name
        # Leave an unchanged file alone, so that nothing rebuilds for no reason.
        if not path.exists() or path.read_text(encoding="utf-8") != text:
            path.write_text(text, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
