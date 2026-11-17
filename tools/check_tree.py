#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Structural checks for the p3a 2.0 tree (tier T2 in docs/2.0/TESTING.md).

    python tools/check_tree.py                 # source checks only
    python tools/check_tree.py --build build   # plus checks on a finished build
    python tools/check_tree.py --build build-test --test-build

Source checks:
  * every component is listed in components/LAYERS.txt with a known layer;
  * a component requires only components in its own layer or below, never
    `main`, and the requirement graph has no cycle;
  * REQUIRES lists are literal (no variables);
  * project code has no __attribute__((weak)), no WHOLE_ARCHIVE, no include
    of a header from main/, and no reference to legacy/;
  * sdkconfig.defaults still carries the silicon-revision guard.

Build checks (--build DIR):
  * the generated sdkconfig says CONFIG_ESP32P4_REV_MIN_FULL=1;
  * p3a.bin is at most 90 percent of the smallest app slot;
  * a release build has no test-hook symbols (p3a_test_*) and does not set
    CONFIG_P3A_TEST_HOOKS; with --test-build the option must be set instead.

Exit status 0 when clean, 1 when anything is wrong. No dependencies beyond
the Python standard library.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

LAYERS = ["util", "board", "storage", "sources", "library", "player", "ui", "net", "main"]
CODE_SUFFIXES = {".c", ".h", ".cpp", ".hpp", ".cc", ".S"}
CMAKE_KEYWORDS = {
    "SRCS", "SRC_DIRS", "EXCLUDE_SRCS", "INCLUDE_DIRS", "PRIV_INCLUDE_DIRS", "LDFRAGMENTS",
    "REQUIRES", "PRIV_REQUIRES", "REQUIRED_IDF_TARGETS", "EMBED_FILES", "EMBED_TXTFILES",
    "KCONFIG", "KCONFIG_PROJBUILD", "WHOLE_ARCHIVE",
}
MAX_IMAGE_FRACTION = 0.90

RE_WEAK = re.compile(r"__attribute__\s*\(\(\s*(__)?weak")
RE_WHOLE_ARCHIVE = re.compile(r"\bWHOLE_ARCHIVE\b")
RE_INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
RE_LEGACY = re.compile(r"\blegacy[/\\]")


def strip_cmake_comments(text: str) -> str:
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())


def parse_requires(cmake_text: str) -> tuple[list[str], list[str]]:
    """Returns (required component names, problems) from idf_component_register calls."""
    text = strip_cmake_comments(cmake_text)
    names: list[str] = []
    problems: list[str] = []
    for call in re.finditer(r"idf_component_register\s*\((.*?)\)", text, re.S):
        tokens = call.group(1).split()
        collecting = False
        for tok in tokens:
            if tok in CMAKE_KEYWORDS:
                collecting = tok in ("REQUIRES", "PRIV_REQUIRES")
                continue
            if collecting:
                name = tok.strip('"')
                if "$" in name:
                    problems.append(f"REQUIRES entry {tok} is not a literal name")
                else:
                    names.append(name)
    return names, problems


def read_layers(path: Path) -> tuple[dict[str, str], set[str], list[str]]:
    layer_of: dict[str, str] = {}
    vendored: set[str] = set()
    problems: list[str] = []
    if not path.exists():
        return layer_of, vendored, [f"{path.as_posix()} is missing"]
    for n, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.split("#", 1)[0].split()
        if not line:
            continue
        if len(line) < 2 or len(line) > 3 or (len(line) == 3 and line[2] != "vendored"):
            problems.append(f"{path.name}:{n}: expected '<component> <layer> [vendored]'")
            continue
        name, layer = line[0], line[1]
        if layer not in LAYERS or layer == "main":
            problems.append(f"{path.name}:{n}: unknown layer '{layer}' for {name}")
            continue
        if name in layer_of:
            problems.append(f"{path.name}:{n}: {name} listed twice")
        layer_of[name] = layer
        if len(line) == 3:
            vendored.add(name)
    return layer_of, vendored, problems


def find_cycle(graph: dict[str, list[str]]) -> list[str] | None:
    state: dict[str, int] = {}
    stack: list[str] = []

    def visit(node: str) -> list[str] | None:
        state[node] = 1
        stack.append(node)
        for nxt in graph.get(node, []):
            if state.get(nxt) == 1:
                return stack[stack.index(nxt):] + [nxt]
            if nxt not in state:
                found = visit(nxt)
                if found:
                    return found
        stack.pop()
        state[node] = 2
        return None

    for node in graph:
        if node not in state:
            found = visit(node)
            if found:
                return found
    return None


def check_sources(root: Path) -> list[str]:
    problems: list[str] = []
    components_dir = root / "components"
    layer_of, vendored, layer_problems = read_layers(components_dir / "LAYERS.txt")
    problems += layer_problems

    present = sorted(p.name for p in components_dir.iterdir() if p.is_dir()) if components_dir.is_dir() else []
    for name in present:
        if name not in layer_of:
            problems.append(f"components/{name} is not listed in components/LAYERS.txt")
    for name in layer_of:
        if name not in present:
            problems.append(f"components/LAYERS.txt lists {name}, which does not exist")

    # Requirement graph and layer rule.
    graph: dict[str, list[str]] = {}
    for name in present:
        cmake = components_dir / name / "CMakeLists.txt"
        if not cmake.exists():
            problems.append(f"components/{name} has no CMakeLists.txt")
            continue
        requires, req_problems = parse_requires(cmake.read_text(encoding="utf-8", errors="replace"))
        problems += [f"components/{name}/CMakeLists.txt: {p}" for p in req_problems]
        graph[name] = [r for r in requires if r in layer_of]
        if "main" in requires:
            problems.append(f"components/{name} requires main; nothing may depend on main")
        if name not in layer_of:
            continue
        own = LAYERS.index(layer_of[name])
        for req in graph[name]:
            if LAYERS.index(layer_of[req]) > own:
                problems.append(
                    f"components/{name} ({layer_of[name]}) requires {req} ({layer_of[req]}), which is a higher layer")
    cycle = find_cycle(graph)
    if cycle:
        problems.append("requirement cycle: " + " -> ".join(cycle))

    # Forbidden patterns in project code.
    scan_dirs = [root / "main"] + [components_dir / n for n in present if n not in vendored]
    for base in scan_dirs:
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if not path.is_file():
                continue
            is_cmake = path.name == "CMakeLists.txt" or path.suffix == ".cmake"
            if path.suffix not in CODE_SUFFIXES and not is_cmake:
                continue
            rel = path.relative_to(root).as_posix()
            text = path.read_text(encoding="utf-8", errors="replace")
            if is_cmake:
                body = strip_cmake_comments(text)
                if RE_WHOLE_ARCHIVE.search(body):
                    problems.append(f"{rel}: WHOLE_ARCHIVE is forbidden")
                if RE_LEGACY.search(body):
                    problems.append(f"{rel}: refers to legacy/")
                if not rel.startswith("main/") and re.search(r"[/\\]main[/\\]", body):
                    problems.append(f"{rel}: refers to main/")
                continue
            if RE_WEAK.search(text):
                problems.append(f"{rel}: __attribute__((weak)) is forbidden")
            for inc in RE_INCLUDE.findall(text):
                norm = inc.replace("\\", "/")
                if norm.startswith("legacy/") or "/legacy/" in norm:
                    problems.append(f"{rel}: includes {inc} from legacy/")
                if not rel.startswith("main/") and (norm.startswith("main/") or "/main/" in norm):
                    problems.append(f"{rel}: includes {inc} from main/")

    # Silicon-revision guard in the defaults file.
    defaults = root / "sdkconfig.defaults"
    lines = set(defaults.read_text(encoding="utf-8").splitlines()) if defaults.exists() else set()
    for needed in ("CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y", "CONFIG_ESP32P4_REV_MIN_1=y"):
        if needed not in lines:
            problems.append(f"sdkconfig.defaults lacks {needed} (silicon-revision guard)")
    return problems


def smallest_app_slot(partitions_csv: Path) -> int | None:
    sizes = []
    for raw in partitions_csv.read_text(encoding="utf-8").splitlines():
        fields = [f.strip() for f in raw.split("#", 1)[0].split(",")]
        if len(fields) >= 5 and fields[1] == "app":
            sizes.append(int(fields[4], 0))
    return min(sizes) if sizes else None


def check_build(root: Path, build: Path, test_build: bool) -> list[str]:
    problems: list[str] = []
    sdkconfig = build / "sdkconfig" if (build / "sdkconfig").exists() else root / "sdkconfig"
    if not sdkconfig.exists():
        return [f"no generated sdkconfig found for {build.as_posix()}"]
    cfg = set(sdkconfig.read_text(encoding="utf-8", errors="replace").splitlines())
    if "CONFIG_ESP32P4_REV_MIN_FULL=1" not in cfg:
        problems.append(f"{sdkconfig.as_posix()}: CONFIG_ESP32P4_REV_MIN_FULL is not 1; this image will not boot on the board")
    hooks = "CONFIG_P3A_TEST_HOOKS=y" in cfg
    if test_build and not hooks:
        problems.append("test build without CONFIG_P3A_TEST_HOOKS=y")
    if not test_build and hooks:
        problems.append("release build has CONFIG_P3A_TEST_HOOKS=y")

    image = build / "p3a.bin"
    slot = smallest_app_slot(root / "partitions.csv")
    if not image.exists():
        problems.append(f"{image.as_posix()} is missing")
    elif slot is None:
        problems.append("partitions.csv has no app partition")
    else:
        size = image.stat().st_size
        limit = int(slot * MAX_IMAGE_FRACTION)
        print(f"image: {size} bytes, {100.0 * size / slot:.1f}% of the {slot}-byte slot (limit {limit})")
        if size > limit:
            problems.append(f"p3a.bin is {size} bytes, above 90% of the {slot}-byte app slot")

    link_map = build / "p3a.map"
    if not test_build and link_map.exists():
        if re.search(r"\bp3a_test_\w+", link_map.read_text(encoding="utf-8", errors="replace")):
            problems.append("release build contains test-hook symbols (p3a_test_*)")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    ap.add_argument("--build", type=Path, help="also check a finished build directory")
    ap.add_argument("--test-build", action="store_true", help="the build directory holds a test build")
    args = ap.parse_args()

    root = args.root.resolve()
    problems = check_sources(root)
    if args.build:
        build = args.build if args.build.is_absolute() else root / args.build
        problems += check_build(root, build, args.test_build)

    for p in problems:
        print(f"FAIL: {p}")
    if problems:
        print(f"{len(problems)} problem(s)")
        return 1
    print("tree checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
