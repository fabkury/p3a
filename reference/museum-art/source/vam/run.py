"""V&A (Victoria & Albert Museum, London) integration demo.

Exercises the four UBI capabilities against the V&A API:

  1. List collections (across three V&A facet axes: ``collection``,
     ``category``, ``venue``).
  2. List artworks inside a collection (range-style pagination via
     ``page`` + ``page_size``).
  3. List artworks via keyword search (``q=``).
  4. Download artworks at exactly 720px on the longest side via IIIF.

Writes a Markdown report to ``output/report.md`` and the downloaded JPEGs
to ``output/images/``.

API reference: https://developers.vam.ac.uk/
IIIF Image API host: ``https://framemark.vam.ac.uk/collections/{id}/...``
  (V&A's IIIF endpoint serves JPEG only.)

Requires: requests
"""

from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import Any, Iterable

import requests

API_BASE = "https://api.vam.ac.uk/v2"
SEARCH_URL = f"{API_BASE}/objects/search"
IIIF_HOST = "https://framemark.vam.ac.uk/collections"

SCRIPT_DIR = Path(__file__).resolve().parent
OUTPUT_DIR = SCRIPT_DIR / "output"
IMAGES_DIR = OUTPUT_DIR / "images"
REPORT_PATH = OUTPUT_DIR / "report.md"

PIXEL_SIZE = 720  # longest side, aspect preserved

# The user picked "all of the above (one each)" for the collection axis, so
# we surface three V&A facet families. Names line up with the cluster keys
# returned by the search API.
FACET_AXES = ("collection", "category", "venue")
FACET_FILTER_PARAM = {
    "collection": "id_collection",
    "category": "id_category",
    "venue": "id_venue",
}

KEYWORDS = ["landscape", "horse"]
PAGE_DEMO_OFFSET = 200
PAGE_DEMO_ROWS = 5
DOWNLOADS_PER_GROUP = 3
TOP_FACETS_PER_AXIS = 5

SESSION = requests.Session()
SESSION.headers.update({"User-Agent": "museum-art/0.1 (+V&A demo)"})


# ---------------------------------------------------------------------------
# V&A API helpers
# ---------------------------------------------------------------------------


def _to_page(offset: int, rows: int) -> tuple[int, int]:
    """V&A only paginates by ``page`` + ``page_size``. To request items
    [offset, offset+rows) we choose ``page_size = rows`` and
    ``page = offset/rows + 1`` — which works exactly when offset is a
    multiple of rows. The script's caller (PAGE_DEMO_OFFSET=200,
    PAGE_DEMO_ROWS=5) satisfies that.
    """
    if rows <= 0:
        raise ValueError("rows must be positive")
    if offset % rows != 0:
        raise ValueError(
            f"V&A pagination requires offset ({offset}) to be a multiple of rows ({rows})"
        )
    return (offset // rows) + 1, rows


def search(
    q: str | None = None,
    offset: int = 0,
    rows: int = 10,
    images_exist: bool = True,
    cluster: bool = False,
    cluster_size: int = TOP_FACETS_PER_AXIS,
    extra: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Hit the V&A search endpoint and return parsed JSON."""
    page, page_size = _to_page(offset, rows) if rows else (1, 1)
    params: dict[str, Any] = {"page": page, "page_size": page_size if rows else 0}
    if q:
        params["q"] = q
    if images_exist:
        params["images_exist"] = 1
    if cluster:
        params["cluster"] = "true"
        params["cluster_size"] = cluster_size
    if extra:
        params.update(extra)
    response = SESSION.get(SEARCH_URL, params=params, timeout=30)
    response.raise_for_status()
    return response.json()


def list_top_facets(
    axis: str, top_n: int = TOP_FACETS_PER_AXIS, images_only: bool = True
) -> list[tuple[str, str, int]]:
    """Return ``[(facet_id, display_value, count), ...]`` for one cluster axis.

    ``images_only`` toggles ``images_exist=1``. The V&A API returns
    ``count=0`` for the ``venue`` axis when ``images_exist=1`` is on (the
    API doesn't compute that combination), so we re-probe ``venue`` without
    the filter purely to populate the listing — actual range pagination and
    downloads still apply ``images_exist=1`` for usable results.
    """
    # The cluster set is fixed; we ask for one record + clusters and read
    # back the desired axis.
    data = search(
        rows=1, images_exist=images_only, cluster=True, cluster_size=top_n
    )
    cluster_block = data.get("clusters", {}).get(axis, {})
    terms = cluster_block.get("terms", []) or []
    out: list[tuple[str, str, int]] = []
    for t in terms:
        if not isinstance(t, dict):
            continue
        out.append((str(t.get("id", "")), str(t.get("value", "")), int(t.get("count", 0))))
    # Drop hits without an id; sort by count desc.
    out = [row for row in out if row[0]]
    out.sort(key=lambda r: -r[2])
    return out[:top_n]


def list_artworks_in_facet(
    axis: str, facet_id: str, offset: int = 0, rows: int = 10
) -> dict[str, Any]:
    """Range-style listing inside one facet bucket (collection/category/venue)."""
    param = FACET_FILTER_PARAM[axis]
    return search(offset=offset, rows=rows, extra={param: facet_id})


def search_keyword(keyword: str, offset: int = 0, rows: int = 10) -> dict[str, Any]:
    """Keyword search across all artworks (image-bearing only)."""
    return search(q=keyword, offset=offset, rows=rows)


# ---------------------------------------------------------------------------
# IIIF image download + JPEG dimension probe
# ---------------------------------------------------------------------------


def iiif_image_url(image_id: str, max_side: int = PIXEL_SIZE) -> str:
    """``!w,h`` = fit within w×h, preserve aspect ratio (longest side becomes max_side)."""
    return f"{IIIF_HOST}/{image_id}/full/!{max_side},{max_side}/0/default.jpg"


def download_image(item: dict[str, Any], dest_dir: Path) -> tuple[Path, str, int] | None:
    image_id = item.get("_primaryImageId") or (
        item.get("_images", {}) or {}
    ).get("_iiif_image_base_url", "").rstrip("/").rsplit("/", 1)[-1]
    if not image_id:
        return None
    system_number = item.get("systemNumber") or "unknown"
    safe_name = "".join(c if c.isalnum() or c in "-_." else "_" for c in str(system_number))
    dest = dest_dir / f"{safe_name}.jpg"
    url = iiif_image_url(image_id)
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
# Field accessors (V&A search records have several "_primary*" fields)
# ---------------------------------------------------------------------------


def get_title(item: dict[str, Any]) -> str:
    title = item.get("_primaryTitle")
    if title:
        return str(title)
    obj_type = item.get("objectType")
    if obj_type:
        return f"({obj_type})"
    return "(untitled)"


def get_artist(item: dict[str, Any]) -> str:
    maker = item.get("_primaryMaker") or {}
    if isinstance(maker, dict):
        name = maker.get("name")
        if name:
            return str(name)
    return "(unknown)"


def get_date(item: dict[str, Any]) -> str:
    return str(item.get("_primaryDate") or "—")


def has_image(item: dict[str, Any]) -> bool:
    if item.get("_primaryImageId"):
        return True
    images = item.get("_images") or {}
    return bool(images.get("_iiif_image_base_url"))


def first_with_image(items: Iterable[dict[str, Any]], n: int) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for it in items:
        if has_image(it):
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
    report.append("# V&A demo run\n\n")
    report.append(f"_Generated: {time.strftime('%Y-%m-%d %H:%M:%S %Z')}_\n\n")
    report.append(
        "This run exercises the four UBI features against the V&A API "
        f"(`{SEARCH_URL}`): list collections (across three facet axes), list "
        "artworks in a collection (range-style pagination via `page`+`page_size`), "
        f"keyword search, and IIIF download at {PIXEL_SIZE}px on the longest side.\n\n"
    )

    # ------------------------------------------------------------------
    # 1. List collections (three facet axes)
    # ------------------------------------------------------------------
    print("[1/4] Listing collections across three facet axes...")
    report.append("## 1. Collections\n\n")
    report.append(
        "V&A doesn't have a single flat *collection* field. Three facet axes "
        "qualify as 'collections' under the UBI's abstraction:\n\n"
        "- **`collection`** — V&A's curatorial collections (closest analogue to SMK's "
        "single `collection` field).\n"
        "- **`category`** — object-type categories (Prints, Photographs, ...).\n"
        "- **`venue`** — the six V&A sites (South Kensington, East, Wedgwood, ...).\n\n"
        "Top values for each axis (image-bearing records only, except `venue` — see note):\n\n"
    )

    facet_top: dict[str, list[tuple[str, str, int]]] = {}
    for axis in FACET_AXES:
        # venue counts are 0 when combined with images_exist=1 — re-probe without it.
        try:
            items = list_top_facets(axis, top_n=TOP_FACETS_PER_AXIS, images_only=(axis != "venue"))
        except requests.HTTPError as exc:
            print(f"  facet axis '{axis}' failed: {exc}")
            items = []
        facet_top[axis] = items
        print(f"      axis '{axis}': {len(items)} term(s)")
        report.append(f"### Axis: `{axis}`\n\n")
        if items:
            report.append("| ID | Value | Count |\n|---|---|---:|\n")
            for fid, val, cnt in items:
                report.append(f"| `{fid}` | {val} | {cnt:,} |\n")
            report.append("\n")
        else:
            report.append("_No facet terms returned._\n\n")

    report.append(
        "_Note on `venue`_: the V&A search API returns `count=0` for venue terms "
        "when `images_exist=1` is also requested (the API doesn't compute that "
        "combination). The listing above for `venue` was therefore fetched without "
        "the image filter; downloads below re-apply it.\n\n"
    )

    # Pick one collection per facet axis. For axes whose top term is missing
    # (e.g. all-zero venue counts), pick by id alone.
    chosen: list[tuple[str, str, str]] = []  # (axis, facet_id, display_value)
    for axis in FACET_AXES:
        items = facet_top.get(axis) or []
        if items:
            chosen.append((axis, items[0][0], items[0][1]))
    print(f"      chose: {[(a, v) for a, _, v in chosen]}")

    # ------------------------------------------------------------------
    # 2. Range pagination inside chosen collections
    # ------------------------------------------------------------------
    print("[2/4] Listing artworks inside collections (range pagination)...")
    report.append("## 2. Artworks in collections (range pagination)\n\n")
    report.append(
        f"Demonstrating range-style pagination by requesting `offset={PAGE_DEMO_OFFSET}, "
        f"rows={PAGE_DEMO_ROWS}` (i.e. artworks {PAGE_DEMO_OFFSET}–"
        f"{PAGE_DEMO_OFFSET + PAGE_DEMO_ROWS - 1}). The V&A API exposes only "
        "`page`+`page_size`, so the adapter computes "
        f"`page = offset/rows + 1 = {PAGE_DEMO_OFFSET // PAGE_DEMO_ROWS + 1}` "
        f"and `page_size = {PAGE_DEMO_ROWS}`.\n\n"
    )
    for axis, facet_id, display in chosen:
        try:
            page = list_artworks_in_facet(
                axis, facet_id, offset=PAGE_DEMO_OFFSET, rows=PAGE_DEMO_ROWS
            )
        except requests.HTTPError as exc:
            print(f"      axis '{axis}' / id {facet_id}: {exc}")
            report.append(f"### {axis}: {display} (`{facet_id}`)\n\n")
            report.append(f"_Request failed: {exc}_\n\n")
            continue
        items = page.get("records", []) or []
        info = page.get("info", {}) or {}
        total = info.get("record_count", 0)
        print(f"      [{axis}] {display}: total={total:,}, returned={len(items)}")
        report.append(f"### `{axis}`: {display} (`{facet_id}`)\n\n")
        report.append(f"Total in this bucket (with images): **{total:,}**.\n\n")
        if items:
            report.append("| Index | System # | Title | Artist | Date |\n|---:|---|---|---|---|\n")
            for i, it in enumerate(items):
                idx = PAGE_DEMO_OFFSET + i
                report.append(
                    f"| {idx} | `{it.get('systemNumber', '—')}` | {get_title(it)} | "
                    f"{get_artist(it)} | {get_date(it)} |\n"
                )
            report.append("\n")
        else:
            report.append("_No items returned at this offset._\n\n")

    # ------------------------------------------------------------------
    # 3. Keyword search
    # ------------------------------------------------------------------
    print("[3/4] Keyword search...")
    report.append("## 3. Keyword search\n\n")
    keyword_first_pages: dict[str, list[dict[str, Any]]] = {}
    for kw in KEYWORDS:
        try:
            page = search_keyword(kw, offset=0, rows=5)
        except requests.HTTPError as exc:
            print(f"      keyword '{kw}' failed: {exc}")
            report.append(f"### Keyword: `{kw}`\n\n_Request failed: {exc}_\n\n")
            continue
        items = page.get("records", []) or []
        info = page.get("info", {}) or {}
        total = info.get("record_count", 0)
        keyword_first_pages[kw] = items
        print(f"      keyword '{kw}': total={total:,}, returned={len(items)}")
        report.append(f"### Keyword: `{kw}`\n\n")
        report.append(f"Total matches (with images): **{total:,}**. First {len(items)}:\n\n")
        if items:
            report.append("| # | System # | Title | Artist | Date |\n|---:|---|---|---|---|\n")
            for i, it in enumerate(items, start=1):
                report.append(
                    f"| {i} | `{it.get('systemNumber', '—')}` | {get_title(it)} | "
                    f"{get_artist(it)} | {get_date(it)} |\n"
                )
            report.append("\n")
        else:
            report.append("_No items returned._\n\n")

    # ------------------------------------------------------------------
    # 4. Downloads — 2 collections + 2 keyword searches, 3 each
    # ------------------------------------------------------------------
    print("[4/4] Downloading artworks at 720px on longest side...")
    report.append("## 4. Downloads\n\n")
    report.append(
        f"Per the demo spec, downloads come from **2 collections** and "
        f"**2 keyword searches**. All images requested via IIIF Image API at "
        f"`{IIIF_HOST}/<id>/full/!{PIXEL_SIZE},{PIXEL_SIZE}/0/default.jpg` "
        f"(fit within {PIXEL_SIZE}×{PIXEL_SIZE}, aspect preserved → longest side "
        f"is exactly {PIXEL_SIZE}). V&A IIIF serves JPEG only.\n\n"
        "Files are saved under `output/images/`.\n\n"
    )

    download_log: list[dict[str, Any]] = []

    def fetch_targets(group_label: str, items: list[dict[str, Any]]) -> None:
        targets = first_with_image(items, DOWNLOADS_PER_GROUP)
        for it in targets:
            obj = it.get("systemNumber") or "unknown"
            print(f"      downloading [{group_label}] {obj}")
            try:
                result = download_image(it, IMAGES_DIR)
            except Exception as exc:
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
                    "error": "no _primaryImageId",
                })
                continue
            dest, url, size = result
            dims = jpeg_dimensions(dest)
            download_log.append({
                "group": group_label, "object": obj,
                "title": get_title(it), "artist": get_artist(it),
                "file": dest.name, "bytes": size, "dims": dims, "url": url,
            })

    # Two collections: take the first two of the chosen facet picks. We re-
    # fetch from offset 0 with a wider window for a healthier download set.
    download_collections = chosen[:2]
    for axis, facet_id, display in download_collections:
        try:
            page = list_artworks_in_facet(axis, facet_id, offset=0, rows=20)
        except requests.HTTPError as exc:
            print(f"      collection [{axis}] {display}: {exc}")
            continue
        fetch_targets(f"collection [{axis}]: {display}", page.get("records", []) or [])

    # Two keyword searches.
    for kw in KEYWORDS:
        try:
            page = search_keyword(kw, offset=0, rows=20)
        except requests.HTTPError as exc:
            print(f"      search '{kw}': {exc}")
            continue
        fetch_targets(f"search: {kw}", page.get("records", []) or [])

    if download_log:
        report.append(
            "| Group | System # | Title | Artist | File | Dimensions | Bytes | URL |\n"
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
    report.append(
        f"- Facet axes surveyed: **{len(FACET_AXES)}** "
        f"(`{'`, `'.join(FACET_AXES)}`)\n"
    )
    report.append(
        f"- Collections sampled in detail: **{len(chosen)}** "
        f"({', '.join(f'[{a}] {v}' for a, _, v in chosen)})\n"
    )
    report.append(
        f"- Collections used for downloads: **{len(download_collections)}** "
        f"({', '.join(f'[{a}] {v}' for a, _, v in download_collections)})\n"
    )
    report.append(f"- Keyword searches: **{len(KEYWORDS)}** ({', '.join(KEYWORDS)})\n")
    report.append(
        f"- Downloads attempted: **{len(download_log)}** "
        f"(succeeded: **{len(successes)}**, with longest-side=={PIXEL_SIZE}: "
        f"**{len(correct_size)}**)\n"
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
