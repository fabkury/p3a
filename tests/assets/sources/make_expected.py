# SPDX-License-Identifier: Apache-2.0
"""What the sources' parsers should make of the recorded responses in this
folder, worked out here in Python: an implementation independent of the C
one the host tests check against it.

    python tests/assets/sources/make_expected.py

writes one `.expected` file per recorded listing. Run it when a response is
recorded again; the results are committed.

The responses were recorded from the live services with the requests the
device makes. They hold no API key. Giphy's are cut down to the fields the
device reads: the service adds analytics tokens and a client id inside each
media address, which belong to the account that recorded them.
"""
import calendar
import json
import re
from pathlib import Path

HERE = Path(__file__).parent
SCREEN = 720
ID = re.compile(r"^[0-9A-Za-z_-]{1,31}$")
DATE = re.compile(r"^(\d{4})-(\d\d)-(\d\d) (\d\d):(\d\d):(\d\d)$")
FILES = {"200.gif": 1, "giphy.gif": 5, "giphy-downsized-medium.gif": 7}


def when(text) -> int:
    m = DATE.match(text or "")
    if not m:
        return 0
    y, mo, d, h, mi, s = map(int, m.groups())
    if y < 1970 or not 1 <= mo <= 12 or not 1 <= d <= 31:
        return 0
    return calendar.timegm((y, mo, d, h, mi, s))


def size(v) -> int:
    try:
        n = int(v)
    except (TypeError, ValueError):
        return 0
    return n if 0 < n <= 65535 else 0


def giphy(name: str, prefer_downsized: bool, now: int) -> None:
    data = json.loads((HERE / f"{name}.json").read_bytes())["data"]
    lines = []
    for gif in data:
        if not isinstance(gif.get("id"), str) or not ID.match(gif["id"]):
            continue
        images = gif.get("images", {})
        fixed = images.get("fixed_height", {})
        file, w, h = 1, size(fixed.get("width")), size(fixed.get("height"))
        dm = images.get("downsized_medium", {})
        last = str(dm.get("url", "")).split("?")[0].split("#")[0].rsplit("/", 1)[-1]
        dw, dh = size(dm.get("width")), size(dm.get("height"))
        if prefer_downsized and last in ("giphy.gif", "giphy-downsized-medium.gif") and dw and dh \
                and dw * 2 <= SCREEN * 3 and dh * 2 <= SCREEN * 3:
            file, w, h = FILES[last], dw, dh
        at = when(gif.get("trending_datetime")) or when(gif.get("import_datetime")) or now
        lines.append(f"{gif['id']} {file} {w} {h} {at}")
    suffix = "downsized" if prefer_downsized else "fixed"
    (HERE / f"{name}.{suffix}.expected").write_text(f"{len(data)}\n" + "\n".join(lines) + "\n", newline="\n")
    print(name, suffix, len(data), "items,", len(lines), "records,",
          sum(1 for line in lines if line.split()[1] != "1"), "downsized")


def klipy_pick(file: dict, webp: bool):
    """(url, width, height) of the file p3a takes, or None."""
    for fmt in (("webp", "gif") if webp else ("gif", "webp")):
        fit = best = None
        for tier in ("hd", "md", "sm", "xs"):
            one = (file.get(tier) or {}).get(fmt)
            if not isinstance(one, dict) or not str(one.get("url", "")).startswith("https://static.klipy.com/"):
                continue
            c = (one["url"], size(one.get("width")), size(one.get("height")))
            if best is None or c[1] * c[2] > best[1] * best[2]:
                best = c
            if c[1] * 2 <= SCREEN * 3 and c[2] * 2 <= SCREEN * 3 and (fit is None or c[1] * c[2] > fit[1] * fit[2]):
                fit = c
        if fit or best:
            return fit or best
    return None


def klipy(name: str, webp: bool) -> None:
    page = json.loads((HERE / f"{name}.json").read_bytes())["data"]
    lines = []
    for item in page["data"]:
        chosen = klipy_pick(item.get("file", {}), webp)
        if item.get("type") == "ad" or not isinstance(item.get("id"), int) or chosen is None:
            continue
        lines.append(f"{item['id']} {chosen[1]} {chosen[2]} {chosen[0]}")
    suffix = "webp" if webp else "gif"
    head = f"{len(page['data'])} {1 if page.get('has_next') else 0}"
    (HERE / f"{name}.{suffix}.expected").write_text(head + "\n" + "\n".join(lines) + "\n", newline="\n")
    print(name, suffix, len(page["data"]), "items,", len(lines), "records")


if __name__ == "__main__":
    for listing in ("klipy_gifs_trending", "klipy_stickers_search", "klipy_gifs_item"):
        klipy(listing, False)
        klipy(listing, True)
    for listing in ("giphy_trending", "giphy_search"):
        giphy(listing, True, 1700000000)
        giphy(listing, False, 1700000000)
