#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Self-test for embed_static.py.

    python tools/test_embed_static.py

Standard library only.
"""
from __future__ import annotations

import gzip
import re
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import embed_static  # noqa: E402


def tree(root: Path, files: dict[str, bytes]) -> Path:
    for name, data in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    return root


def embedded(text: str, index: int) -> bytes:
    body = re.search(rf"s_file_{index}\[\] = \{{(.*?)\}};", text, re.S).group(1)
    return bytes(int(b, 16) for b in re.findall(r"0x([0-9a-f]{2})", body))


class Embed(unittest.TestCase):
    def test_files_are_gzipped_listed_in_order_and_round_trip(self):
        page = b"<html>" + b"hello " * 200 + b"</html>"
        with tempfile.TemporaryDirectory() as tmp:
            src = tree(Path(tmp) / "src", {"setup/index.html": page, "app.js": b"let a = 1;\n", "logo.png": b"\x89PNG123"})
            text = embed_static.generate(embed_static.collect(src))
        rows = re.findall(r'\{"(/[^"]+)", s_file_(\d+), (\d+), "([^"]+)", "([^,]+)", (true|false)\}', text)
        self.assertEqual(["/app.js", "/logo.png", "/setup/index.html"], [r[0] for r in rows])
        self.assertIn("p3a_static_file_count = 3;", text)
        by_path = {r[0]: r for r in rows}
        html = embedded(text, int(by_path["/setup/index.html"][1]))
        self.assertEqual(page, gzip.decompress(html))
        self.assertLess(len(html), len(page) // 4)
        self.assertEqual(str(len(html)), by_path["/setup/index.html"][2])
        self.assertEqual("text/html; charset=utf-8", by_path["/setup/index.html"][3])
        self.assertEqual("true", by_path["/setup/index.html"][5])
        self.assertEqual(f'\\"{zlib.crc32(html):08x}\\"', by_path["/setup/index.html"][4])
        # A PNG is stored as it is.
        self.assertEqual(b"\x89PNG123", embedded(text, int(by_path["/logo.png"][1])))
        self.assertEqual("false", by_path["/logo.png"][5])

    def test_output_is_reproducible(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = tree(Path(tmp) / "src", {"a.html": b"<p>x</p>"})
            first = embed_static.generate(embed_static.collect(src))
            (src / "a.html").touch()
            self.assertEqual(first, embed_static.generate(embed_static.collect(src)))

    def test_an_empty_folder_gives_an_empty_table_that_compiles(self):
        with tempfile.TemporaryDirectory() as tmp:
            text = embed_static.generate(embed_static.collect(Path(tmp)))
        self.assertIn("p3a_static_file_count = 0;", text)
        self.assertIn("{NULL, NULL, 0, NULL, NULL, false},", text)

    def test_refusals(self):
        for name, expect in (("notes.md", "no content type"), ("my page.html", "plain ASCII"),
                             ("x" * 70 + ".js", "at most"), ("café.html", "plain ASCII")):
            with tempfile.TemporaryDirectory() as tmp:
                src = tree(Path(tmp) / "src", {name: b"x"})
                with self.assertRaises(embed_static.EmbedError) as caught:
                    embed_static.collect(src)
                self.assertIn(expect, str(caught.exception))

    def test_main(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = tree(Path(tmp) / "src", {"a.html": b"<p>x</p>"})
            out = Path(tmp) / "out"
            self.assertEqual(0, embed_static.main(["e", str(src), str(out)]))
            target = out / "p3a_static_gen.c"
            stamp = target.stat().st_mtime_ns
            self.assertEqual(0, embed_static.main(["e", str(src), str(out)]))
            self.assertEqual(stamp, target.stat().st_mtime_ns, "an unchanged output is left alone")
            self.assertEqual(1, embed_static.main(["e", str(src / "missing"), str(out)]))
            (src / "bad.xyz").write_bytes(b"x")
            self.assertEqual(1, embed_static.main(["e", str(src), str(out)]))


if __name__ == "__main__":
    unittest.main()
