"""Wellcome Collection integration demo.

Exercises the four UBI capabilities against the Wellcome Catalogue API:

  1. List collections across **four** Wellcome facet axes
     (workType, genres, subjects, contributors) — pulled live via the API's
     ``aggregations=...`` parameter, scoped to image-bearing works.
  2. List artworks inside a collection (range-style pagination via
     ``page`` + ``pageSize``).
  3. List artworks via keyword search (``query=``).
  4. Download artworks at exactly 720px on the longest side via IIIF.

Writes a Markdown report to ``output/report.md`` and the downloaded JPEGs
to ``output/images/``.

API reference: https://developers.wellcomecollection.org/api
IIIF Image API host: ``https://iiif.wellcomecollection.org/image/{vid}/...``

Wellcome's catalogue mixes books, manuscripts, archives, and images.
``items.locations.locationType=iiif-image`` filters down to the
~83K works that have a IIIF Image service. Per-work IIIF Image URLs come
from ``items[].locations[]`` where ``locationType.id == 'iiif-image'``;
to surface those, the work request must pass ``include=items``.

Requires: requests
"""

from __future__ import annotations

import re
import sys
import time
from pathlib import Path
from typing import Any, Iterable

import requests

API_BASE = "https://api.wellcomecollection.org/catalogue/v2"
WORKS_URL = f"{API_BASE}/works"

SCRIPT_DIR = Path(__file__).resolve().parent
OUTPUT_DIR = SCRIPT_DIR / "output"
IMAGES_DIR = OUTPUT_DIR / "images"
REPORT_PATH = OUTPUT_DIR / "report.md"

PIXEL_SIZE = 720  # longest side, aspect preserved

# Each axis: (display_name, aggregations key, filter param). For axes whose
# aggregation buckets carry a structured ``data.id`` (workType), we filter
# by id; otherwise we filter by label.
FACET_AXES: tuple[dict[str, str], ...] = (
    {"name": "workType", "agg": "workType", "filter": "workType", "key": "id"},
    {"name": "genres", "agg": "genres.label", "filter": "genres.label", "key": "label"},
    {"name": "subjects", "agg": "subjects.label", "filter": "subjects.label", "key": "label"},
    {
        "name": "contributors",
        "agg": "contributors.agent.label",
        "filter": "contributors.agent.label",
        "key": "label",
    },
)

KEYWORDS = ["landscape", "horse"]
PAGE_DEMO_OFFSET = 200
PAGE_DEMO_ROWS = 5
DOWNLOADS_PER_GROUP = 3
TOP_TERMS_PER_AXIS = 5

# Filter to image-bearing works for every demo request that should yield
# downloadable results.
IIIF_IMAGE_FILTER = {"items.locations.locationType": "iiif-image"}

# What we want included on every results record so the report and download
# step have what they need.
RESULT_INCLUDES = "items,contributors,production,subjects,genres"

SESSION = requests.Session()
SESSION.headers.update({"User-Agent": "museum-art/0.1 (+Wellcome demo)"})


# ---------------------------------------------------------------------------
# Wellcome Catalogue API helpers
# ---------------------------------------------------------------------------


def _to_page(offset: int, rows: int) -> tuple[int, int]:
    """Wellcome paginates by ``page`` + ``pageSize``. To request items
    [offset, offset+rows) we use ``pageSize=rows`` and
    ``page=offset/rows + 1`` — requires offset % rows == 0.
    """
    if rows <= 0:
        raise ValueError("rows must be positive")
    if offset % rows != 0:
        raise ValueError(
            f"Wellcome pagination requires offset ({offset}) to be a multiple of rows ({rows})"
        )
    return (offset // rows) + 1, rows


def _get(params: dict[str, Any]) -> dict[str, Any]:
    response = SESSION.get(WORKS_URL, params=params, timeout=30)
    response.raise_for_status()
    return response.json()


def list_facets() -> dict[str, list[dict[str, Any]]]:
    """One request returns all four aggregations at once. Each bucket's
    ``data`` carries the term metadata; for workType it has ``id`` + ``label``,
    other axes carry just ``label``.
    """
    aggs = ",".join(a["agg"] for a in FACET_AXES)
    params = {
        "pageSize": 1,
        "aggregations": aggs,
        **IIIF_IMAGE_FILTER,
    }
    data = _get(params)
    out: dict[str, list[dict[str, Any]]] = {}
    aggregations = data.get("aggregations") or {}
    for axis in FACET_AXES:
        block = aggregations.get(axis["agg"]) or {}
        buckets = block.get("buckets") or []
        rows: list[dict[str, Any]] = []
        for b in buckets[:TOP_TERMS_PER_AXIS]:
            d = b.get("data") or {}
            rows.append({
                "id": d.get("id"),
                "label": d.get("label"),
                "count": int(b.get("count", 0)),
            })
        out[axis["name"]] = rows
    return out


def list_artworks_by_term(
    filter_param: str, term_value: str, offset: int = 0, rows: int = 10
) -> dict[str, Any]:
    """Range-style listing of works in one facet bucket (image-bearing only)."""
    page, page_size = _to_page(offset, rows)
    params: dict[str, Any] = {
        "page": page,
        "pageSize": page_size,
        "include": RESULT_INCLUDES,
        filter_param: term_value,
        **IIIF_IMAGE_FILTER,
    }
    return _get(params)


def search_keyword(keyword: str, offset: int = 0, rows: int = 10) -> dict[str, Any]:
    """Keyword search across image-bearing works."""
    page, page_size = _to_page(offset, rows)
    params: dict[str, Any] = {
        "query": keyword,
        "page": page,
        "pageSize": page_size,
        "include": RESULT_INCLUDES,
        **IIIF_IMAGE_FILTER,
    }
    return _get(params)


# ---------------------------------------------------------------------------
# IIIF image URL extraction + download + JPEG dimension probe
# ---------------------------------------------------------------------------


_INFO_JSON_RE = re.compile(r"^(?P<base>https://iiif\.wellcomecollection\.org/image/[^/]+)/info\.json$")


def iiif_base_for_work(work: dict[str, Any]) -> str | None:
    """Find the first IIIF Image API base URL on a work's items[].locations[]."""
    for item in work.get("items") or []:
        for loc in item.get("locations") or []:
            ltype = (loc.get("locationType") or {}).get("id")
            url = loc.get("url") or ""
            if ltype == "iiif-image" and url:
                m = _INFO_JSON_RE.match(url)
                if m:
                    return m.group("base")
                # Fallback: strip a trailing /info.json if pattern didn't match.
                if url.endswith("/info.json"):
                    return url[: -len("/info.json")]
                return url.rstrip("/")
    return None


def iiif_image_url(base: str, max_side: int = PIXEL_SIZE) -> str:
    return f"{base}/full/!{max_side},{max_side}/0/default.jpg"


def download_image(item: dict[str, Any], dest_dir: Path) -> tuple[Path, str, int] | None:
    base = iiif_base_for_work(item)
    if not base:
        return None
    work_id = item.get("id") or "unknown"
    safe_name = "".join(c if c.isalnum() or c in "-_." else "_" for c in str(work_id))
    dest = dest_dir / f"{safe_name}.jpg"
    url = iiif_image_url(base)
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
        if marker in (0xD8, 0xD9):
            continue
        if marker == 0xDA:
            return None
        if 0xC0 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
            i += 3
            height = (data[i] << 8) | data[i + 1]
            width = (data[i + 2] << 8) | data[i + 3]
            return width, height
        if i + 1 >= len(data):
            return None
        length = (data[i] << 8) | data[i + 1]
        i += length
    return None


# ---------------------------------------------------------------------------
# Field accessors
# ---------------------------------------------------------------------------


def get_title(work: dict[str, Any]) -> str:
    title = work.get("title")
    if isinstance(title, str) and title:
        # Wellcome titles are sometimes paragraph-length; trim for the report.
        if len(title) > 140:
            return title[:137] + "..."
        return title
    return "(untitled)"


def get_artist(work: dict[str, Any]) -> str:
    contribs = work.get("contributors") or []
    if isinstance(contribs, list) and contribs:
        first = contribs[0] or {}
        agent = first.get("agent") or {}
        label = agent.get("label")
        if label:
            return str(label)
    return "(unknown)"


def get_date(work: dict[str, Any]) -> str:
    prod = work.get("production") or []
    if isinstance(prod, list) and prod:
        first = prod[0] or {}
        for d in first.get("dates") or []:
            label = d.get("label")
            if label:
                return str(label)
    return "—"


def first_with_image(items: Iterable[dict[str, Any]], n: int) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for it in items:
        if iiif_base_for_work(it):
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
    report.append("# Wellcome Collection demo run\n\n")
    report.append(f"_Generated: {time.strftime('%Y-%m-%d %H:%M:%S %Z')}_\n\n")
    report.append(
        "This run exercises the four UBI features against the Wellcome "
        f"Catalogue API (`{WORKS_URL}`): list collections (across four facet "
        "axes), list artworks in a collection (range-style pagination via "
        "`page`+`pageSize`), keyword search, and IIIF download at "
        f"{PIXEL_SIZE}px on the longest side. All listings are filtered to "
        "image-bearing works via `items.locations.locationType=iiif-image`.\n\n"
    )

    # ------------------------------------------------------------------
    # 1. List collections (four facet axes via aggregations)
    # ------------------------------------------------------------------
    print("[1/4] Listing collections across four facet axes...")
    report.append("## 1. Collections\n\n")
    report.append(
        "Wellcome's catalogue is a unified library/archive index. Four facet "
        "axes qualify as 'collections' under the UBI's abstraction; all are "
        "filterable via direct query parameters:\n\n"
        "- **`workType`** — coarse format (Pictures, Digital Images, ...).\n"
        "- **`genres`** — fine-grained genres (Engraving, Lithography, ...).\n"
        "- **`subjects`** — what's depicted (Botany, Human anatomy, ...).\n"
        "- **`contributors`** — creator agents.\n\n"
        "All counts below are computed by the Wellcome API's "
        "`aggregations=...` parameter, scoped to image-bearing works.\n\n"
    )

    facet_top = list_facets()
    for axis in FACET_AXES:
        name = axis["name"]
        rows = facet_top.get(name) or []
        print(f"      axis '{name}': {len(rows)} term(s)")
        report.append(f"### Axis: `{name}`\n\n")
        report.append(f"_Filter parameter_: `{axis['filter']}`.\n\n")
        if rows:
            cols = "| Filter value | Label | Count |\n|---|---|---:|\n"
            report.append(cols)
            for r in rows:
                fv = r.get(axis["key"]) or r.get("label") or "—"
                report.append(f"| `{fv}` | {r.get('label', '—')} | {r.get('count', 0):,} |\n")
            report.append("\n")
        else:
            report.append("_No facet terms returned._\n\n")

    # Pick the top (largest) term per axis. Aggregations come back sorted by
    # count desc already, so `rows[0]` is the biggest.
    chosen: list[tuple[str, str, str, str, int]] = []  # (axis, filter_param, filter_value, label, count)
    for axis in FACET_AXES:
        rows = facet_top.get(axis["name"]) or []
        if not rows:
            continue
        top = rows[0]
        filter_value = top.get(axis["key"]) or top.get("label")
        if not filter_value:
            continue
        chosen.append((
            axis["name"], axis["filter"], str(filter_value),
            str(top.get("label") or filter_value), int(top.get("count") or 0)
        ))
    print(f"      chose for pagination:")
    for a, _, _, l, c in chosen:
        print(f"        [{a}] {l} ({c:,} works)")

    # ------------------------------------------------------------------
    # 2. Range pagination inside chosen terms
    # ------------------------------------------------------------------
    print("[2/4] Listing artworks inside collections (range pagination)...")
    report.append("## 2. Artworks in collections (range pagination)\n\n")
    report.append(
        f"Demonstrating range-style pagination by requesting `offset={PAGE_DEMO_OFFSET}, "
        f"rows={PAGE_DEMO_ROWS}` (i.e. works {PAGE_DEMO_OFFSET}–"
        f"{PAGE_DEMO_OFFSET + PAGE_DEMO_ROWS - 1}). Wellcome exposes only "
        "`page`+`pageSize`, so the adapter computes "
        f"`page = offset/pageSize + 1 = {PAGE_DEMO_OFFSET // PAGE_DEMO_ROWS + 1}` "
        f"and `pageSize = {PAGE_DEMO_ROWS}`.\n\n"
    )
    for axis_name, filter_param, filter_value, label, count in chosen:
        if count >= PAGE_DEMO_OFFSET + PAGE_DEMO_ROWS:
            offset = PAGE_DEMO_OFFSET
            note = ""
        else:
            offset = 0
            note = (
                f" (term has only {count:,} works — falling back to "
                f"`offset=0` instead of {PAGE_DEMO_OFFSET})"
            )
        try:
            page = list_artworks_by_term(filter_param, filter_value, offset=offset, rows=PAGE_DEMO_ROWS)
        except requests.HTTPError as exc:
            print(f"      [{axis_name}] {label}: {exc}")
            report.append(f"### `{axis_name}`: {label}\n\n")
            report.append(f"_Request failed: {exc}_\n\n")
            continue
        items = page.get("results", []) or []
        total = page.get("totalResults", 0)
        print(f"      [{axis_name}] {label}: total={total:,}, returned={len(items)}, offset={offset}")
        report.append(f"### `{axis_name}`: {label}\n\n")
        report.append(
            f"Total in this bucket: **{total:,}** "
            f"(filter: `{filter_param}={filter_value}`).{note}\n\n"
        )
        if items:
            report.append("| Index | ID | Title | Artist | Date |\n|---:|---|---|---|---|\n")
            for i, it in enumerate(items):
                idx = offset + i
                report.append(
                    f"| {idx} | `{it.get('id', '—')}` | {get_title(it)} | "
                    f"{get_artist(it)} | {get_date(it)} |\n"
                )
            report.append("\n")
        else:
            report.append("_No items returned._\n\n")

    # ------------------------------------------------------------------
    # 3. Keyword search
    # ------------------------------------------------------------------
    print("[3/4] Keyword search...")
    report.append("## 3. Keyword search\n\n")
    for kw in KEYWORDS:
        try:
            page = search_keyword(kw, offset=0, rows=5)
        except requests.HTTPError as exc:
            print(f"      keyword '{kw}' failed: {exc}")
            report.append(f"### Keyword: `{kw}`\n\n_Request failed: {exc}_\n\n")
            continue
        items = page.get("results", []) or []
        total = page.get("totalResults", 0)
        print(f"      keyword '{kw}': total={total:,}, returned={len(items)}")
        report.append(f"### Keyword: `{kw}`\n\n")
        report.append(f"Total matches (with images): **{total:,}**. First {len(items)}:\n\n")
        if items:
            report.append("| # | ID | Title | Artist | Date |\n|---:|---|---|---|---|\n")
            for i, it in enumerate(items, start=1):
                report.append(
                    f"| {i} | `{it.get('id', '—')}` | {get_title(it)} | "
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
        f"**2 keyword searches**. All images requested via the Wellcome IIIF "
        f"Image API at "
        f"`https://iiif.wellcomecollection.org/image/<vid>/full/!{PIXEL_SIZE},{PIXEL_SIZE}/0/default.jpg` "
        f"(fit within {PIXEL_SIZE}×{PIXEL_SIZE}, aspect preserved → longest "
        f"side is exactly {PIXEL_SIZE}). The IIIF base URL is recovered from "
        "each work's `items[].locations[]` where `locationType.id == "
        "'iiif-image'`.\n\nFiles are saved under `output/images/`.\n\n"
    )

    download_log: list[dict[str, Any]] = []

    def fetch_targets(group_label: str, items: list[dict[str, Any]]) -> None:
        targets = first_with_image(items, DOWNLOADS_PER_GROUP)
        for it in targets:
            obj = it.get("id") or "unknown"
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
                    "error": "no IIIF image base on items[]",
                })
                continue
            dest, url, size = result
            dims = jpeg_dimensions(dest)
            download_log.append({
                "group": group_label, "object": obj,
                "title": get_title(it), "artist": get_artist(it),
                "file": dest.name, "bytes": size, "dims": dims, "url": url,
            })

    download_collections = chosen[:2]
    for axis_name, filter_param, filter_value, label, _count in download_collections:
        try:
            page = list_artworks_by_term(filter_param, filter_value, offset=0, rows=20)
        except requests.HTTPError as exc:
            print(f"      collection [{axis_name}] {label}: {exc}")
            continue
        fetch_targets(f"[{axis_name}] {label}", page.get("results", []) or [])

    for kw in KEYWORDS:
        try:
            page = search_keyword(kw, offset=0, rows=20)
        except requests.HTTPError as exc:
            print(f"      search '{kw}': {exc}")
            continue
        fetch_targets(f"search: {kw}", page.get("results", []) or [])

    if download_log:
        report.append(
            "| Group | ID | Title | Artist | File | Dimensions | Bytes | URL |\n"
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

    axis_names = ", ".join(f"`{a['name']}`" for a in FACET_AXES)
    report.append("## Run summary\n\n")
    report.append(f"- Facet axes surveyed: **{len(FACET_AXES)}** ({axis_names})\n")
    report.append(
        f"- Collections sampled in detail: **{len(chosen)}** "
        f"({', '.join(f'[{a}] {l}' for a, _, _, l, _ in chosen)})\n"
    )
    report.append(
        f"- Collections used for downloads: **{len(download_collections)}** "
        f"({', '.join(f'[{a}] {l}' for a, _, _, l, _ in download_collections)})\n"
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
