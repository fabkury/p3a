"""AIC (Art Institute of Chicago) integration demo.

Exercises the four UBI capabilities against the AIC API:

  1. List collections across **all seven** AIC facet axes
     (departments, classifications, subjects, themes, galleries,
     exhibitions, artwork-types).
  2. List artworks inside a collection (range-style pagination via
     ``page`` + ``limit``).
  3. List artworks via keyword search (``q=``).
  4. Download artworks at exactly 720px on the longest side via IIIF.

Writes a Markdown report to ``output/report.md`` and the downloaded JPEGs
to ``output/images/``.

API reference: https://api.artic.edu/docs/
IIIF Image API host: ``https://www.artic.edu/iiif/2/{id}/...``

Notes on AIC's "collection" model: there is no single ``collection``
field. AIC instead exposes **seven** orthogonal facet vocabularies. Six
support direct artwork-level filtering via the search API's
``query[term][...]`` clause; the seventh — ``exhibitions`` — is text-only
on the artwork side (``exhibition_history``) and so is list-only here.

Requires: requests
"""

from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import Any, Iterable

import requests

API_BASE = "https://api.artic.edu/api/v1"
LIST_URL = f"{API_BASE}/artworks"
SEARCH_URL = f"{API_BASE}/artworks/search"
IIIF_HOST = "https://www.artic.edu/iiif/2"

SCRIPT_DIR = Path(__file__).resolve().parent
OUTPUT_DIR = SCRIPT_DIR / "output"
IMAGES_DIR = OUTPUT_DIR / "images"
REPORT_PATH = OUTPUT_DIR / "report.md"

PIXEL_SIZE = 720  # longest side, aspect preserved

# Each axis spec describes (display_name, listing_endpoint, listing_params,
# artwork_filter_field). ``artwork_filter_field=None`` marks the axis as
# list-only — artwork-level filtering not supported via the search API.
#
# - ``departments``       — 16 curatorial departments, IDs like ``PC-3``.
# - ``classifications``   — object-type sub-vocabulary inside ``category-terms``,
#                           IDs like ``TM-13``.
# - ``subjects``          — depicted-subject sub-vocabulary, IDs like ``TM-8658``.
# - ``themes``            — curatorial themes (~21), IDs like ``PC-109``;
#                           filtered via the master ``category_ids`` field.
# - ``galleries``         — 179 physical galleries, numeric IDs.
# - ``exhibitions``       — 6,252 exhibitions; LIST-ONLY (no structured ID
#                           field on artworks).
# - ``artwork-types``     — 45 object-type buckets, numeric IDs.
FACET_AXES: tuple[dict[str, Any], ...] = (
    {
        "name": "departments",
        "endpoint": f"{API_BASE}/departments",
        "params": {},
        "filter_field": "department_id",
    },
    {
        "name": "classifications",
        "endpoint": f"{API_BASE}/category-terms/search",
        "params": {"query[term][subtype]": "classification"},
        "filter_field": "classification_id",
    },
    {
        "name": "subjects",
        "endpoint": f"{API_BASE}/category-terms/search",
        "params": {"query[term][subtype]": "subject"},
        "filter_field": "subject_id",
    },
    {
        "name": "themes",
        "endpoint": f"{API_BASE}/category-terms/search",
        "params": {"query[term][subtype]": "theme"},
        # Themes share the master category_ids field (PC-IDs) with departments.
        "filter_field": "category_ids",
    },
    {
        "name": "galleries",
        "endpoint": f"{API_BASE}/galleries",
        "params": {},
        "filter_field": "gallery_id",
    },
    {
        "name": "exhibitions",
        "endpoint": f"{API_BASE}/exhibitions",
        "params": {},
        # exhibition_history on artworks is free text; no structured term.
        "filter_field": None,
    },
    {
        "name": "artwork-types",
        "endpoint": f"{API_BASE}/artwork-types",
        "params": {},
        "filter_field": "artwork_type_id",
    },
)

KEYWORDS = ["landscape", "horse"]
PAGE_DEMO_OFFSET = 200
PAGE_DEMO_ROWS = 5
DOWNLOADS_PER_GROUP = 3
TOP_TERMS_PER_AXIS = 5

# Fields requested for compact artwork records (search returns a thin record
# by default; we ask for what the report needs).
ARTWORK_FIELDS = (
    "id,title,image_id,artist_title,date_display,department_title,"
    "classification_title,artwork_type_title,gallery_title,"
    "is_public_domain,thumbnail"
)

SESSION = requests.Session()
SESSION.headers.update({
    # AIC asks integrators to send an identifying User-Agent — see
    # https://api.artic.edu/docs/#identifying-yourself.
    "AIC-User-Agent": "museum-art/0.1 (pub@kury.dev)",
    "User-Agent": "museum-art/0.1 (+AIC demo)",
})


# ---------------------------------------------------------------------------
# AIC API helpers
# ---------------------------------------------------------------------------


def _to_page(offset: int, rows: int) -> tuple[int, int]:
    """AIC paginates by ``page`` + ``limit``. To request items
    [offset, offset+rows) we use ``limit=rows`` and ``page=offset/rows + 1`` —
    requires offset % rows == 0.
    """
    if rows <= 0:
        raise ValueError("rows must be positive")
    if offset % rows != 0:
        raise ValueError(
            f"AIC pagination requires offset ({offset}) to be a multiple of rows ({rows})"
        )
    return (offset // rows) + 1, rows


def _get(url: str, params: dict[str, Any]) -> dict[str, Any]:
    response = SESSION.get(url, params=params, timeout=30)
    response.raise_for_status()
    return response.json()


def list_facet_terms(axis: dict[str, Any], top_n: int = TOP_TERMS_PER_AXIS) -> list[dict[str, Any]]:
    """Return up to ``top_n`` term records for one facet axis.

    The endpoint underneath the axis returns rows in storage order, not
    "most-used" order — AIC does not surface a per-term artwork count on
    the term-listing endpoints. So we just take the first ``top_n`` rows
    and accept that "top" here means "first listed".
    """
    params = dict(axis["params"])
    params.update({"limit": top_n, "page": 1})
    data = _get(axis["endpoint"], params)
    rows = data.get("data", []) or []
    return rows[:top_n]


def list_artworks_by_term(
    filter_field: str, term_id: Any, offset: int = 0, rows: int = 10
) -> dict[str, Any]:
    """Range-style listing of artworks filtered by a structured term."""
    page, limit = _to_page(offset, rows)
    params = {
        "page": page,
        "limit": limit,
        "fields": ARTWORK_FIELDS,
        f"query[term][{filter_field}]": term_id,
    }
    return _get(SEARCH_URL, params)


def term_artwork_count(filter_field: str, term_id: Any) -> int:
    """Cheap count probe for one term (limit=1)."""
    params = {"page": 1, "limit": 1, "fields": "id", f"query[term][{filter_field}]": term_id}
    return int(_get(SEARCH_URL, params).get("pagination", {}).get("total", 0))


def search_keyword(keyword: str, offset: int = 0, rows: int = 10) -> dict[str, Any]:
    """Keyword search via Elasticsearch ``query_string`` (strict filter, not the
    permissive ``q=`` relevance-only parameter — ``q=`` returns the entire
    corpus ranked by score, which gives a misleading 'total matches' count).
    """
    page, limit = _to_page(offset, rows)
    params = {
        "page": page,
        "limit": limit,
        "fields": ARTWORK_FIELDS,
        "query[query_string][query]": keyword,
    }
    return _get(SEARCH_URL, params)


# ---------------------------------------------------------------------------
# IIIF image download + JPEG dimension probe
# ---------------------------------------------------------------------------


def iiif_image_url(image_id: str, max_side: int = PIXEL_SIZE) -> str:
    return f"{IIIF_HOST}/{image_id}/full/!{max_side},{max_side}/0/default.jpg"


def download_image(item: dict[str, Any], dest_dir: Path) -> tuple[Path, str, int] | None:
    image_id = item.get("image_id")
    if not image_id:
        return None
    artwork_id = item.get("id") or "unknown"
    safe_name = "".join(c if c.isalnum() or c in "-_." else "_" for c in str(artwork_id))
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
# Field accessors
# ---------------------------------------------------------------------------


def get_title(item: dict[str, Any]) -> str:
    return str(item.get("title") or "(untitled)")


def get_artist(item: dict[str, Any]) -> str:
    return str(item.get("artist_title") or "(unknown)")


def get_date(item: dict[str, Any]) -> str:
    return str(item.get("date_display") or "—")


def first_with_image(items: Iterable[dict[str, Any]], n: int) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for it in items:
        if it.get("image_id"):
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
    report.append("# AIC demo run\n\n")
    report.append(f"_Generated: {time.strftime('%Y-%m-%d %H:%M:%S %Z')}_\n\n")
    report.append(
        "This run exercises the four UBI features against the AIC API "
        f"(`{SEARCH_URL}`): list collections (across **seven** facet axes), "
        "list artworks in a collection (range-style pagination via "
        "`page`+`limit`), keyword search, and IIIF download at "
        f"{PIXEL_SIZE}px on the longest side.\n\n"
    )

    # ------------------------------------------------------------------
    # 1. List collections (seven facet axes)
    # ------------------------------------------------------------------
    print("[1/4] Listing collections across seven facet axes...")
    report.append("## 1. Collections\n\n")
    report.append(
        "AIC has no single `collection` field. Seven orthogonal vocabularies "
        "qualify as 'collections' under the UBI's abstraction. Six of them "
        "support artwork-level filtering via the search API's structured "
        "`query[term][...]`; **`exhibitions`** is list-only (the artwork side "
        "stores `exhibition_history` as free text, not a structured ID).\n\n"
    )

    facet_top: dict[str, list[dict[str, Any]]] = {}
    for axis in FACET_AXES:
        name = axis["name"]
        try:
            terms = list_facet_terms(axis, top_n=TOP_TERMS_PER_AXIS)
        except requests.HTTPError as exc:
            print(f"  axis '{name}' failed: {exc}")
            terms = []
        facet_top[name] = terms
        filterable = "yes" if axis["filter_field"] else "no (list-only)"
        print(f"      axis '{name}': {len(terms)} term(s); filterable={filterable}")
        report.append(f"### Axis: `{name}`\n\n")
        report.append(f"_Artwork-level filterable_: **{filterable}**")
        if axis["filter_field"]:
            report.append(f" (via `query[term][{axis['filter_field']}]`)")
        report.append(".\n\n")
        if terms:
            report.append("| ID | Title |\n|---|---|\n")
            for t in terms:
                report.append(f"| `{t.get('id', '—')}` | {t.get('title', '—')} |\n")
            report.append("\n")
        else:
            report.append("_No terms returned._\n\n")

    # Pick one term per filterable axis: the listing endpoints don't expose
    # per-term artwork counts and return rows in storage order, so the first
    # term is often a long-tail value (e.g. artwork-type "TBM Equipment" with
    # 0 items). We probe each candidate's count and pick the largest.
    print("      probing term sizes to pick the largest per axis...")
    chosen: list[tuple[str, str, Any, str, int]] = []  # (axis, filter_field, id, title, count)
    for axis in FACET_AXES:
        if not axis["filter_field"]:
            continue
        terms = facet_top.get(axis["name"]) or []
        best: tuple[Any, str, int] | None = None
        for t in terms:
            try:
                c = term_artwork_count(axis["filter_field"], t.get("id"))
            except requests.HTTPError:
                continue
            if best is None or c > best[2]:
                best = (t.get("id"), str(t.get("title", "—")), c)
        if best is None:
            continue
        chosen.append((axis["name"], axis["filter_field"], best[0], best[1], best[2]))
    print(f"      chose for pagination:")
    for a, _, _, t, c in chosen:
        print(f"        [{a}] {t} ({c:,} artworks)")

    # ------------------------------------------------------------------
    # 2. Range pagination inside chosen terms
    # ------------------------------------------------------------------
    print("[2/4] Listing artworks inside collections (range pagination)...")
    report.append("## 2. Artworks in collections (range pagination)\n\n")
    report.append(
        f"Demonstrating range-style pagination by requesting `offset={PAGE_DEMO_OFFSET}, "
        f"rows={PAGE_DEMO_ROWS}` (i.e. artworks {PAGE_DEMO_OFFSET}–"
        f"{PAGE_DEMO_OFFSET + PAGE_DEMO_ROWS - 1}). The AIC API exposes only "
        "`page`+`limit`, so the adapter computes "
        f"`page = offset/limit + 1 = {PAGE_DEMO_OFFSET // PAGE_DEMO_ROWS + 1}` "
        f"and `limit = {PAGE_DEMO_ROWS}`.\n\n"
        "_AIC's search caps Elasticsearch results at 10,000 records — beyond "
        "that, range queries fail and a browse-by-id strategy is needed. The "
        "demo offsets stay well below the cap._\n\n"
    )

    for axis_name, filter_field, term_id, term_title, term_count in chosen:
        # Use the standard offset when the term has enough items; otherwise
        # fall back to offset=0 so the demo still shows real data.
        if term_count >= PAGE_DEMO_OFFSET + PAGE_DEMO_ROWS:
            offset = PAGE_DEMO_OFFSET
            note = ""
        else:
            offset = 0
            note = (
                f" (term has only {term_count:,} artworks — falling back to "
                f"`offset=0` instead of {PAGE_DEMO_OFFSET})"
            )
        try:
            page = list_artworks_by_term(filter_field, term_id, offset=offset, rows=PAGE_DEMO_ROWS)
        except requests.HTTPError as exc:
            print(f"      [{axis_name}] {term_title}: {exc}")
            report.append(f"### `{axis_name}`: {term_title} (`{term_id}`)\n\n")
            report.append(f"_Request failed: {exc}_\n\n")
            continue
        items = page.get("data", []) or []
        total = page.get("pagination", {}).get("total", 0)
        print(f"      [{axis_name}] {term_title}: total={total:,}, returned={len(items)}, offset={offset}")
        report.append(f"### `{axis_name}`: {term_title} (`{term_id}`)\n\n")
        report.append(
            f"Total in this bucket: **{total:,}** "
            f"(filter: `{filter_field}={term_id}`).{note}\n\n"
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
        items = page.get("data", []) or []
        total = page.get("pagination", {}).get("total", 0)
        print(f"      keyword '{kw}': total={total:,}, returned={len(items)}")
        report.append(f"### Keyword: `{kw}`\n\n")
        report.append(f"Total matches: **{total:,}**. First {len(items)}:\n\n")
        if items:
            report.append(
                "| # | ID | Title | Artist | Date |\n"
                "|---:|---|---|---|---|\n"
            )
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
        f"**2 keyword searches**. All images requested via IIIF Image API at "
        f"`{IIIF_HOST}/<image_id>/full/!{PIXEL_SIZE},{PIXEL_SIZE}/0/default.jpg` "
        f"(fit within {PIXEL_SIZE}×{PIXEL_SIZE}, aspect preserved → longest side "
        f"is exactly {PIXEL_SIZE}).\n\n"
        "Files are saved under `output/images/`.\n\n"
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
                    "error": "no image_id",
                })
                continue
            dest, url, size = result
            dims = jpeg_dimensions(dest)
            download_log.append({
                "group": group_label, "object": obj,
                "title": get_title(it), "artist": get_artist(it),
                "file": dest.name, "bytes": size, "dims": dims, "url": url,
            })

    # Two collections: take the first two filterable picks. Re-fetch from
    # offset 0 with a wider window for a healthier download set.
    download_collections = chosen[:2]
    for axis_name, filter_field, term_id, term_title, _term_count in download_collections:
        try:
            page = list_artworks_by_term(filter_field, term_id, offset=0, rows=20)
        except requests.HTTPError as exc:
            print(f"      collection [{axis_name}] {term_title}: {exc}")
            continue
        fetch_targets(f"[{axis_name}] {term_title}", page.get("data", []) or [])

    # Two keyword searches.
    for kw in KEYWORDS:
        try:
            page = search_keyword(kw, offset=0, rows=20)
        except requests.HTTPError as exc:
            print(f"      search '{kw}': {exc}")
            continue
        fetch_targets(f"search: {kw}", page.get("data", []) or [])

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

    report.append("## Run summary\n\n")
    axis_names = ", ".join(f"`{a['name']}`" for a in FACET_AXES)
    report.append(
        f"- Facet axes surveyed: **{len(FACET_AXES)}** ({axis_names})\n"
    )
    report.append(
        f"- Filterable axes: **{sum(1 for a in FACET_AXES if a['filter_field'])}** of "
        f"{len(FACET_AXES)}\n"
    )
    report.append(
        f"- Collections sampled in detail: **{len(chosen)}** "
        f"({', '.join(f'[{a}] {t}' for a, _, _, t, _ in chosen)})\n"
    )
    report.append(
        f"- Collections used for downloads: **{len(download_collections)}** "
        f"({', '.join(f'[{a}] {t}' for a, _, _, t, _ in download_collections)})\n"
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
