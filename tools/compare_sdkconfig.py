#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compares the generated 2.0 release sdkconfig with the one 1.x shipped.

    python tools/compare_sdkconfig.py [generated-sdkconfig]

2.0 rebuilds its configuration from sdkconfig.defaults instead of carrying
the 1.x file over. This check proves that nothing was lost on the way: every
setting that both files define must have the same value, except for the
differences listed in EXPECTED below, each with its reason.

It fails on a difference that is not listed, and also on a listed difference
that no longer exists, so that the list stays true.

Settings that only one file defines are not compared: they belong to
components that 2.0 has not brought in yet, or has dropped. They are counted
in the summary.

The reference is legacy/sdkconfig. When legacy/ is emptied at the end of the
migration (M6.2) this script goes with it.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REFERENCE = ROOT / "legacy" / "sdkconfig"

# setting -> why 2.0 differs from 1.x
EXPECTED = {
    "CONFIG_IDF_INIT_VERSION": "informational: the IDF version that first generated the file",
    "CONFIG_HTTPD_WS_SUPPORT": "WebSocket support was only for PICO-8, which 2.0 drops",
    "CONFIG_BSP_SPIFFS_MOUNT_POINT": "1.x pointed the BSP's SPIFFS mount at /webui; 2.0 has no such partition",
    # These three are switched on by esp_wifi_remote / esp_hosted. They must
    # leave this list at the co-processor step of M1.2, when that link returns.
    "CONFIG_MBEDTLS_CMAC_C": "selected by the Wi-Fi stack, which 2.0 has not brought in yet",
    "CONFIG_PM_SLEEP_FUNC_IN_IRAM": "selected by the Wi-Fi stack, which 2.0 has not brought in yet",
    "CONFIG_PM_SLP_IRAM_OPT": "selected by the Wi-Fi stack, which 2.0 has not brought in yet",
}


def load(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        m = re.match(r"(CONFIG_[A-Za-z0-9_]+)=(.*)", line)
        if m:
            values[m.group(1)] = m.group(2)
            continue
        m = re.match(r"# (CONFIG_[A-Za-z0-9_]+) is not set", line)
        if m:
            values[m.group(1)] = "n"
    return values


def compare(old: dict[str, str], new: dict[str, str], expected: dict[str, str]) -> list[str]:
    problems = []
    differing = {k for k in old.keys() & new.keys() if old[k] != new[k]}
    for key in sorted(differing - expected.keys()):
        problems.append(f"{key}: 1.x has {old[key]}, 2.0 has {new[key]} (not in the expected list)")
    for key in sorted(expected.keys() - differing):
        state = "no longer differs" if key in old and key in new else "is no longer defined by both"
        problems.append(f"{key}: listed as an expected difference but {state}; remove it from EXPECTED")
    return problems


def main() -> int:
    generated = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "sdkconfig"
    if not REFERENCE.exists():
        print(f"{REFERENCE} is gone; nothing to compare against")
        return 0
    if not generated.exists():
        print(f"FAIL: {generated} does not exist; build the release configuration first")
        return 1
    old, new = load(REFERENCE), load(generated)
    common = old.keys() & new.keys()
    problems = compare(old, new, EXPECTED)
    print(f"{len(common)} settings defined by both; {len(old) - len(common)} only in 1.x, "
          f"{len(new) - len(common)} only in 2.0; {len(EXPECTED)} expected differences")
    for p in problems:
        print(f"FAIL: {p}")
    if problems:
        return 1
    print("configuration matches 1.x where both define a setting")
    return 0


if __name__ == "__main__":
    sys.exit(main())
