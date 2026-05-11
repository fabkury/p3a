"""SMK (Statens Museum for Kunst) integration demo.

Exercises the four UBI capabilities against the SMK API:

  1. List collections.
  2. List artworks inside a collection (with range-style pagination).
  3. List artworks via keyword search.
  4. Download artworks at exactly 720px on the longest side via IIIF.

Writes a Markdown report to ``output/report.md`` and the downloaded JPEGs
to ``output/images/``.

API reference: https://api.smk.dk/api/v1/docs/
IIIF Image API: ``{image_iiif_id}/{region}/{size}/{rotation}/{quality}.{format}``

Requires: requests
"""

from __future__ import annotations

import sys
import time
from collections import Counter
from pathlib import Path
from typing import Any, Iterable

import requests

API_BASE = "https://api.smk.dk/api/v1"
SEARCH_URL = f"{API_BASE}/art/search"

SCRIPT_DIR = Path(__file__).resolve().parent
OUTPUT_DIR = SCRIPT_DIR / "output"
IMAGES_DIR = OUTPUT_DIR / "images"
REPORT_PATH = OUTPUT_DIR / "report.md"

PIXEL_SIZE = 720  # longest side, aspect preserved

KEYWORDS = ["landscape", "horse"]
PAGE_DEMO_OFFSET = 200
PAGE_DEMO_ROWS = 5
DOWNLOADS_PER_GROUP = 3

SESSION = requests.Session()
SESSION.headers.update({"User-Agent": "museum-art/0.1 (+SMK demo)"})


# ---------------------------------------------------------------------------
# SMK API helpers
# ---------------------------------------------------------------------------


def search(
    keys: str = "*",
    offset: int = 0,
    rows: int = 10,
    filters: str | None = None,
    facets: str | None = None,
) -> dict[str, Any]:
    """Hit the SMK search endpoint and return parsed JSON."""
    params: dict[str, Any] = {"keys": keys, "offset": offset, "rows": rows}
    if filters:
        params["filters"] = filters
    if facets:
        params["facets"] = facets
    response = SESSION.get(SEARCH_URL, params=params, timeout=30)
    response.raise_for_status()
    return response.json()


def _parse_facet_pairs(raw: Any) -> list[tuple[str, int]]:
    """SMK can return facets as alternating [name, count, ...] arrays, lists of pairs, or dicts."""
    pairs: list[tuple[str, int]] = []
    if isinstance(raw, dict):
        pairs = [(str(name), int(count)) for name, count in raw.items()]
    elif isinstance(raw, list) and raw:
        head = raw[0]
        if isinstance(head, (str, int)) and len(raw) % 2 == 0:
            it = iter(raw)
            pairs = [(str(name), int(count)) for name, count in zip(it, it)]
        else:
            for entry in raw:
                if isinstance(entry, dict):
                    name = entry.get("name") or entry.get("value") or entry.get("key")
                    count = entry.get("count") or entry.get("doc_count") or 0
                    if name is not None:
                        pairs.append((str(name), int(count)))
                elif isinstance(entry, (list, tuple)) and len(entry) == 2:
                    name, count = entry
                    pairs.append((str(name), int(count)))
    return pairs


def list_collections(top_n: int = 10) -> list[tuple[str, int]]:
    """Discover collection facet values; fall back to sampling if facets are unavailable."""
    data = search(keys="*", rows=0, facets="collection")
    pairs = _parse_facet_pairs(data.get("facets", {}).get("collection", []))
    if not pairs:
        print("  facets API returned nothing; sampling artworks to aggregate collections")
        counter: Counter[str] = Counter()
        for offset in range(0, 1000, 100):
            page = search(keys="*", offset=offset, rows=100)
            for item in page.get("items", []) or []:
                col = item.get("collection")
                if isinstance(col, str):
                    counter[col] += 1
                elif isinstance(col, list):
                    for c in col:
                        if isinstance(c, str):
                            counter[c] += 1
        pairs = counter.most_common()
    pairs.sort(key=lambda x: -x[1])
    return pairs[:top_n]


def list_artworks_in_collection(
    collection_name: str,
    offset: int = 0,
    rows: int = 10,
    require_image: bool = False,
) -> dict[str, Any]:
    """Range-style listing inside one collection. Optionally filter to artworks with images."""
    parts = [f"[collection:{collection_name}]"]
    if require_image:
        parts.append("[has_image:true]")
    return search(keys="*", offset=offset, rows=rows, filters=",".join(parts))


def search_keyword(keyword: str, offset: int = 0, rows: int = 10, require_image: bool = False) -> dict[str, Any]:
    """Keyword search across all artworks."""
    filters = "[has_image:true]" if require_image else None
    return search(keys=keyword, offset=offset, rows=rows, filters=filters)


# ---------------------------------------------------------------------------
# IIIF image download + JPEG dimension probe
# ---------------------------------------------------------------------------


def iiif_image_url(image_iiif_id: str, max_side: int = PIXEL_SIZE) -> str:
    # ``!w,h`` = fit within w x h, preserve aspect ratio (longest side becomes max_side).
    # NOTE: SMK's info.json advertises webp output, but the IIPImage backend returns
    # "IIIF :: unsupported output format" (HTTP 400) for any .webp request — the server
    # was apparently not compiled with libwebp. JPEG and PNG both serve fine. Sticking
    # with JPEG; clients that want smaller files should re-encode locally.
    return f"{image_iiif_id}/full/!{max_side},{max_side}/0/default.jpg"


def download_image(item: dict[str, Any], dest_dir: Path) -> tuple[Path, str, int] | None:
    iiif_id = item.get("image_iiif_id")
    if not iiif_id:
        return None
    object_number = item.get("object_number") or item.get("id") or "unknown"
    safe_name = "".join(c if c.isalnum() or c in "-_." else "_" for c in str(object_number))
    dest = dest_dir / f"{safe_name}.jpg"
    url = iiif_image_url(iiif_id)
    resp = SESSION.get(url, timeout=60)
    resp.raise_for_status()
    dest.write_bytes(resp.content)
    return dest, url, len(resp.content)


def jpeg_dimensions(path: Path) -> tuple[int, int] | None:
    """Stdlib-only JPEG dimension reader (parses SOF marker). Avoids a Pillow dependency."""
    data = path.read_bytes()
    if data[:2] != b"\xff\xd8":
        return None
    i = 2
    while i < len(data) - 9:
        if data[i] != 0xFF:
            return None
        while i < len(data) and data[i] == 0xFF:
            i += 1
        if i >= len(data):
            return None
        marker = data[i]
        i += 1
        if marker in (0xD8, 0xD9):  # SOI/EOI
            continue
        if marker == 0xDA:  # SOS — image data follows
            return None
        if 0xC0 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
            i += 3  # length(2) + precision(1)
            height = (data[i] << 8) | data[i + 1]
            width = (data[i + 2] << 8) | data[i + 3]
            return width, height
        if i + 1 >= len(data):
            return None
        length = (data[i] << 8) | data[i + 1]
        i += length
    return None


# ---------------------------------------------------------------------------
# Field accessors (SMK records are nested + multilingual)
# ---------------------------------------------------------------------------


def get_title(item: dict[str, Any]) -> str:
    titles = item.get("titles") or []
    if isinstance(titles, list):
        for entry in titles:
            if isinstance(entry, dict):
                t = entry.get("title")
                if t:
                    return str(t)
            elif isinstance(entry, str):
                return entry
    if isinstance(titles, str):
        return titles
    return "(untitled)"


def get_artist(item: dict[str, Any]) -> str:
    prod = item.get("production") or []
    if isinstance(prod, list):
        for entry in prod:
            if isinstance(entry, dict):
                creator = entry.get("creator") or entry.get("creator_forename") or entry.get("creator_surname")
                if creator:
                    return str(creator)
    return "(unknown)"


def get_collection(item: dict[str, Any]) -> str:
    col = item.get("collection")
    if isinstance(col, list) and col:
        return ", ".join(str(c) for c in col)
    if isinstance(col, str):
        return col
    return "—"


def first_with_image(items: Iterable[dict[str, Any]], n: int) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for it in items:
        if it.get("image_iiif_id"):
            out.append(it)
            if len(out) >= n:
                break
    return out


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main() -> int:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    IMAGES_DIR.mkdir(parents=True, exist_ok=True)
    started = time.time()

    report: list[str] = []
    report.append("# SMK demo run\n\n")
    report.append(f"_Generated: {time.strftime('%Y-%m-%d %H:%M:%S %Z')}_\n\n")
    report.append(
        "This run exercises the four UBI features against the SMK API: list collections, "
        "list artworks in a collection (range pagination), keyword search, and IIIF download "
        f"at {PIXEL_SIZE}px on the longest side.\n\n"
    )

    # ------------------------------------------------------------------
    # 1. List collections
    # ------------------------------------------------------------------
    print("[1/4] Listing collections...")
    collections = list_collections(top_n=10)
    print(f"      found {len(collections)} collection(s)")
    report.append("## 1. Collections\n\n")
    if collections:
        report.append("| Collection | Count |\n|---|---:|\n")
        for name, count in collections:
            report.append(f"| {name} | {count:,} |\n")
        report.append("\n")
    else:
        report.append("No collection facet values returned and sampling produced none.\n\n")

    if len(collections) >= 2:
        chosen = [collections[0][0], collections[1][0]]
    else:
        chosen = ["Den Kongelige Kobberstiksamling", "Den Kongelige Maleri- og Skulptursamling"]
    print(f"      chose: {chosen}")

    # ------------------------------------------------------------------
    # 2. List artworks inside chosen collections (range pagination)
    # ------------------------------------------------------------------
    print("[2/4] Listing artworks inside collections (range pagination)...")
    report.append("## 2. Artworks in collections (range pagination)\n\n")
    report.append(
        f"Demonstrating range-style pagination by requesting `offset={PAGE_DEMO_OFFSET}, "
        f"rows={PAGE_DEMO_ROWS}` (i.e. artworks {PAGE_DEMO_OFFSET}-{PAGE_DEMO_OFFSET + PAGE_DEMO_ROWS - 1}).\n\n"
    )
    collection_pages: dict[str, dict[str, Any]] = {}
    for name in chosen:
        page = list_artworks_in_collection(
            name, offset=PAGE_DEMO_OFFSET, rows=PAGE_DEMO_ROWS, require_image=True
        )
        items = page.get("items", []) or []
        total = page.get("found", 0)
        collection_pages[name] = page
        print(f"      {name}: total={total:,}, returned={len(items)}")
        report.append(f"### {name}\n\n")
        report.append(f"Total in collection (with images): **{total:,}**.\n\n")
        if items:
            report.append("| Index | Object # | Title | Artist |\n|---:|---|---|---|\n")
            for i, it in enumerate(items):
                idx = PAGE_DEMO_OFFSET + i
                report.append(
                    f"| {idx} | `{it.get('object_number', '—')}` | {get_title(it)} | {get_artist(it)} |\n"
                )
            report.append("\n")
        else:
            report.append("_No items returned at this offset._\n\n")

    # ------------------------------------------------------------------
    # 3. Keyword search
    # ------------------------------------------------------------------
    print("[3/4] Keyword search...")
    report.append("## 3. Keyword search\n\n")
    keyword_pages: dict[str, dict[str, Any]] = {}
    for kw in KEYWORDS:
        page = search_keyword(kw, offset=0, rows=5, require_image=True)
        items = page.get("items", []) or []
        total = page.get("found", 0)
        keyword_pages[kw] = page
        print(f"      keyword '{kw}': total={total:,}, returned={len(items)}")
        report.append(f"### Keyword: `{kw}`\n\n")
        report.append(f"Total matches (with images): **{total:,}**. First 5:\n\n")
        if items:
            report.append("| # | Object # | Title | Artist | Collection |\n|---:|---|---|---|---|\n")
            for i, it in enumerate(items, start=1):
                report.append(
                    f"| {i} | `{it.get('object_number', '—')}` | {get_title(it)} | "
                    f"{get_artist(it)} | {get_collection(it)} |\n"
                )
            report.append("\n")
        else:
            report.append("_No items returned._\n\n")

    # ------------------------------------------------------------------
    # 4. Downloads
    # ------------------------------------------------------------------
    print("[4/4] Downloading artworks at 720px on longest side...")
    report.append("## 4. Downloads\n\n")
    report.append(
        f"All images requested via IIIF Image API with size `!{PIXEL_SIZE},{PIXEL_SIZE}` "
        "(fit within a 720x720 box, aspect ratio preserved -> longest side is exactly 720).\n\n"
    )
    report.append("Files are saved under `output/images/`.\n\n")

    download_log: list[dict[str, Any]] = []

    def fetch_targets(group_label: str, items: list[dict[str, Any]]) -> None:
        targets = first_with_image(items, DOWNLOADS_PER_GROUP)
        for it in targets:
            obj = it.get("object_number") or it.get("id") or "unknown"
            print(f"      downloading [{group_label}] {obj}")
            try:
                result = download_image(it, IMAGES_DIR)
            except Exception as exc:  # network / HTTP / disk
                download_log.append({
                    "group": group_label, "object": obj,
                    "title": get_title(it), "artist": get_artist(it),
                    "error": f"{type(exc).__name__}: {exc}",
                })
                continue
            if result is None:
                download_log.append({
                    "group": group_label, "object": obj,
                    "title": get_title(it), "artist": get_artist(it),
                    "error": "no image_iiif_id",
                })
                continue
            dest, url, size = result
            dims = jpeg_dimensions(dest)
            download_log.append({
                "group": group_label, "object": obj,
                "title": get_title(it), "artist": get_artist(it),
                "file": dest.name, "bytes": size, "dims": dims, "url": url,
            })

    # Two collections (re-fetch from offset 0 for nicer display + safer download set).
    for name in chosen:
        page = list_artworks_in_collection(name, offset=0, rows=20, require_image=True)
        fetch_targets(f"collection: {name}", page.get("items", []) or [])

    # Two keyword searches (re-fetch with require_image=True if needed for more candidates).
    for kw in KEYWORDS:
        page = search_keyword(kw, offset=0, rows=20, require_image=True)
        fetch_targets(f"search: {kw}", page.get("items", []) or [])

    if download_log:
        report.append(
            "| Group | Object # | Title | Artist | File | Dimensions | Bytes | URL |\n"
            "|---|---|---|---|---|---|---:|---|\n"
        )
        for entry in download_log:
            file_cell = entry.get("file", "—")
            dims = entry.get("dims")
            dims_cell = f"{dims[0]}×{dims[1]}" if dims else (entry.get("error") or "—")
            bytes_cell = f"{entry.get('bytes', 0):,}" if "bytes" in entry else "—"
            url_cell = entry.get("url", "—")
            report.append(
                f"| {entry['group']} | `{entry['object']}` | {entry['title']} | "
                f"{entry['artist']} | `{file_cell}` | {dims_cell} | {bytes_cell} | {url_cell} |\n"
            )
        report.append("\n")

    # ------------------------------------------------------------------
    # Verification + summary
    # ------------------------------------------------------------------
    successes = [e for e in download_log if "bytes" in e]
    correct_size = [e for e in successes if e.get("dims") and PIXEL_SIZE in e["dims"]]
    elapsed = time.time() - started

    report.append("## Run summary\n\n")
    report.append(f"- Collections discovered: **{len(collections)}**\n")
    report.append(f"- Collections sampled in detail: **{len(chosen)}** ({', '.join(chosen)})\n")
    report.append(f"- Keyword searches: **{len(KEYWORDS)}** ({', '.join(KEYWORDS)})\n")
    report.append(
        f"- Downloads attempted: **{len(download_log)}** "
        f"(succeeded: **{len(successes)}**, with longest-side==720: **{len(correct_size)}**)\n"
    )
    report.append(f"- Elapsed: **{elapsed:.1f}s**\n")

    REPORT_PATH.write_text("".join(report), encoding="utf-8")
    print(f"\nWrote {REPORT_PATH}")
    print(f"Wrote {len(successes)} image(s) to {IMAGES_DIR}")
    print(
        f"Of those, {len(correct_size)}/{len(successes)} have a side equal to "
        f"{PIXEL_SIZE}px (verifying the IIIF size request)."
    )
    return 0 if successes else 1


if __name__ == "__main__":
    sys.exit(main())
