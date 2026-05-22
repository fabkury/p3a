"""Smithsonian Open Access integration demo.

Exercises the four UBI capabilities against the Smithsonian Open Access API:

  1. List collections (the 21 Smithsonian "units" / sub-museums).
  2. List artworks inside a unit (range-style pagination via ``start`` + ``rows``).
  3. List artworks via keyword search (``q=``).
  4. Download artworks at exactly 720px on the longest side via IIIF.

Plus four Smithsonian-specific probes that p3a's firmware adapter needs
answered before we touch any C code:

  A. Per-unit IIIF coverage stat — for each art-bearing unit, sample 50
     hits and report % with a usable ``idsId`` and % whose ``info.json``
     actually returns 200.
  B. Field-shape audit — concretely document the JSON paths that hold the
     IDS ID, rights string, title, artist, and date.
  C. Rate-limit probe — controlled burst against api.si.edu to capture
     the actual 429 wire format including ``Retry-After``.
  D. Filter A/B — compare result counts for ``unit_code:SAAM`` alone vs
     ``+ AND online_visual_material:true`` vs ``+ AND usage:CC0`` to
     establish the production query.

Writes a Markdown report to ``output/report.md`` and the downloaded JPEGs
to ``output/images/``.

API reference: https://edan.si.edu/openaccess/docs/
IIIF Image host: ``https://ids.si.edu/ids/iiif/{idsId}/{region}/{size}/{rotation}/{quality}.{format}``

Key: defaults to ``DEMO_KEY`` (api.data.gov public test key — 30 req/min,
50/day per IP — fine for this script). Override via ``SI_API_KEY`` env
var if rate-limited.

Requires: requests
"""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path
from typing import Any, Iterable

import requests

API_BASE = "https://api.si.edu/openaccess/api/v1.0"
SEARCH_URL = f"{API_BASE}/search"
IIIF_PREFIX = "https://ids.si.edu/ids/iiif/"

SCRIPT_DIR = Path(__file__).resolve().parent
OUTPUT_DIR = SCRIPT_DIR / "output"
IMAGES_DIR = OUTPUT_DIR / "images"
REPORT_PATH = OUTPUT_DIR / "report.md"

PIXEL_SIZE = 720


def _resolve_api_key() -> str:
    """Resolve API key from (in order): SI_API_KEY env var, sibling .api_key file, DEMO_KEY."""
    env_key = os.environ.get("SI_API_KEY")
    if env_key:
        return env_key.strip()
    key_file = Path(__file__).resolve().parent / ".api_key"
    if key_file.is_file():
        return key_file.read_text(encoding="utf-8").strip()
    return "DEMO_KEY"


API_KEY = _resolve_api_key()
# Never write the actual key into reports or logs — only a redacted label.
API_KEY_LABEL = "DEMO_KEY" if API_KEY == "DEMO_KEY" else "(user-provided key, redacted)"

# The Smithsonian units, separated by art-bearing vs the rest. The art-bearing
# set is what p3a will likely wire; the rest are listed so we can compare counts
# and confirm the curation choice. Codes taken from the official Smithsonian/
# OpenAccess GitHub repo.
#
# Note: FSG (Freer Gallery + Arthur M. Sackler Gallery, now collectively the
# National Museum of Asian Art) returns 0 items in the openaccess API as of
# 2026-05 — the Freer/Sackler collection has not yet been added to the open-
# access dataset. Kept here for empirical record; will be excluded from the
# firmware unit list unless that changes.
ART_UNITS: tuple[tuple[str, str], ...] = (
    ("SAAM",   "Smithsonian American Art Museum"),
    ("NPG",    "National Portrait Gallery"),
    ("CHNDM",  "Cooper Hewitt, Smithsonian Design Museum"),
    ("NMAI",   "National Museum of the American Indian"),
    ("FSG",    "Freer Gallery of Art + Sackler (National Museum of Asian Art)"),
    ("NMAfA",  "National Museum of African Art"),
    ("HMSG",   "Hirshhorn Museum and Sculpture Garden"),
    ("NMAAHC", "National Museum of African American History and Culture"),
)
OTHER_UNITS: tuple[tuple[str, str], ...] = (
    ("NMNH",        "National Museum of Natural History"),
    ("NMAH",        "National Museum of American History"),
    ("NASM",        "National Air and Space Museum"),
    ("NPM",         "National Postal Museum"),
    ("ACM",         "Anacostia Community Museum"),
    ("NZP",         "Smithsonian's National Zoo & Conservation Biology Institute"),
    ("SIA",         "Smithsonian Institution Archives"),
    ("AAA",         "Archives of American Art"),
    ("SIL",         "Smithsonian Libraries"),
    ("NAA",         "National Anthropological Archives"),
    ("EEPA",        "Eliot Elisofon Photographic Archives"),
    ("HSFA",        "Human Studies Film Archives"),
    ("HAC",         "Smithsonian Gardens"),
    ("FBR",         "Smithsonian Field Book Project"),
    ("CFCHFOLKLIFE","Ralph Rinzler Folklife Archives and Collections"),
)

KEYWORDS = ["landscape", "horse"]
PAGE_DEMO_OFFSET = 200
PAGE_DEMO_ROWS = 5
DOWNLOADS_PER_UNIT = 3
COVERAGE_SAMPLE_SIZE = 50
COVERAGE_IIIF_PROBE_SIZE = 10  # how many to actually probe info.json for
RATE_LIMIT_BURST = 30
DEEP_PAGE_OFFSETS = (0, 100, 500, 1000, 10000)

SESSION = requests.Session()
SESSION.headers.update({"User-Agent": "museum-art/0.1 (+Smithsonian demo)"})


# ---------------------------------------------------------------------------
# Smithsonian API helpers
# ---------------------------------------------------------------------------


def search(
    q: str = "*",
    start: int = 0,
    rows: int = 10,
    extra: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Hit the Smithsonian Open Access search endpoint and return parsed JSON.

    The API takes ``q`` (Solr-style), ``start``, ``rows``, and ``api_key``.
    Filters are AND-concatenated into ``q`` (Smithsonian does NOT expose
    Solr's ``fq=`` — everything goes through ``q``).
    """
    params: dict[str, Any] = {
        "api_key": API_KEY,
        "q": q,
        "start": start,
        "rows": rows,
    }
    if extra:
        params.update(extra)
    response = SESSION.get(SEARCH_URL, params=params, timeout=30)
    response.raise_for_status()
    return response.json()


def _count_for_query(q: str) -> int:
    """Cheap count probe (rows=0, read rowCount)."""
    data = search(q=q, start=0, rows=0)
    return int(data.get("response", {}).get("rowCount", 0))


def unit_count(unit_code: str, online_only: bool = True) -> int:
    """Item count for a unit, optionally restricted to items with online media."""
    if online_only:
        q = f"unit_code:{unit_code} AND online_visual_material:true"
    else:
        q = f"unit_code:{unit_code}"
    try:
        return _count_for_query(q)
    except requests.HTTPError as exc:
        print(f"      count probe failed for {unit_code}: {exc}")
        return -1


def list_unit_items(unit_code: str, start: int = 0, rows: int = 10) -> dict[str, Any]:
    return search(
        q=f"unit_code:{unit_code} AND online_visual_material:true",
        start=start,
        rows=rows,
    )


def search_keyword(keyword: str, start: int = 0, rows: int = 10) -> dict[str, Any]:
    # Restrict to items with online visual media so search results match
    # what the firmware would actually be able to display.
    return search(
        q=f'("{keyword}") AND online_visual_material:true',
        start=start,
        rows=rows,
    )


# ---------------------------------------------------------------------------
# Field accessors (Smithsonian records are deeply nested + heterogeneous)
# ---------------------------------------------------------------------------


def _safe_get(obj: Any, *path: str | int) -> Any:
    cur = obj
    for key in path:
        if cur is None:
            return None
        if isinstance(key, int):
            if isinstance(cur, list) and 0 <= key < len(cur):
                cur = cur[key]
            else:
                return None
        else:
            if isinstance(cur, dict):
                cur = cur.get(key)
            else:
                return None
    return cur


def get_ids_id(item: dict[str, Any]) -> str | None:
    """Pull the IIIF ``idsId`` from the first online media entry.

    The media block can be either a dict (when there's one media) or a
    list (when there are multiple). Both are handled.
    """
    media = _safe_get(item, "content", "descriptiveNonRepeating", "online_media", "media")
    if isinstance(media, list):
        for m in media:
            if isinstance(m, dict) and m.get("idsId"):
                return str(m["idsId"])
    elif isinstance(media, dict):
        ids = media.get("idsId")
        if ids:
            return str(ids)
    return None


def get_title(item: dict[str, Any]) -> str:
    t = item.get("title")
    if isinstance(t, str) and t.strip():
        return t.strip()
    # Some records put title inside content.descriptiveNonRepeating.title.content
    nested = _safe_get(item, "content", "descriptiveNonRepeating", "title", "content")
    if isinstance(nested, str) and nested.strip():
        return nested.strip()
    return "(untitled)"


def get_artist(item: dict[str, Any]) -> str:
    """Best-effort artist extraction. Smithsonian records use freetext.name
    (an array of {label, content}), or sometimes indexedStructured.name.
    """
    names = _safe_get(item, "content", "freetext", "name")
    if isinstance(names, list):
        for entry in names:
            if isinstance(entry, dict):
                label = (entry.get("label") or "").lower()
                content = entry.get("content")
                if isinstance(content, str) and any(
                    k in label for k in ("artist", "maker", "creator", "designer", "photographer", "manufacturer")
                ):
                    return content
        # Fallback: first name entry of any kind
        for entry in names:
            if isinstance(entry, dict) and entry.get("content"):
                return str(entry["content"])
    structured = _safe_get(item, "content", "indexedStructured", "name")
    if isinstance(structured, list) and structured:
        first = structured[0]
        if isinstance(first, str):
            return first
        if isinstance(first, dict) and first.get("content"):
            return str(first["content"])
    return "(unknown)"


def get_date(item: dict[str, Any]) -> str:
    dates = _safe_get(item, "content", "freetext", "date")
    if isinstance(dates, list):
        for entry in dates:
            if isinstance(entry, dict) and entry.get("content"):
                return str(entry["content"])
    structured = _safe_get(item, "content", "indexedStructured", "date")
    if isinstance(structured, list) and structured:
        return str(structured[0])
    return "—"


def get_rights(item: dict[str, Any]) -> str:
    rights = _safe_get(item, "content", "freetext", "objectRights")
    if isinstance(rights, list):
        for entry in rights:
            if isinstance(entry, dict) and entry.get("content"):
                return str(entry["content"])
    # Some records use creditLine or usage info instead
    usage = _safe_get(item, "content", "descriptiveNonRepeating", "metadata_usage", "access")
    if isinstance(usage, str):
        return usage
    return "(none)"


def get_unit_label(item: dict[str, Any]) -> str:
    """Unit code stored on the record (echo back for cross-check)."""
    return str(_safe_get(item, "content", "descriptiveNonRepeating", "unit_code") or "—")


# ---------------------------------------------------------------------------
# IIIF download + JPEG dimension probe
# ---------------------------------------------------------------------------


def iiif_image_url(ids_id: str, max_side: int = PIXEL_SIZE) -> str:
    return f"{IIIF_PREFIX}{ids_id}/full/!{max_side},{max_side}/0/default.jpg"


def iiif_info_url(ids_id: str) -> str:
    return f"{IIIF_PREFIX}{ids_id}/info.json"


def probe_iiif_info(ids_id: str) -> tuple[bool, int]:
    """Verify info.json returns 200. Returns (ok, status_code)."""
    try:
        resp = SESSION.get(iiif_info_url(ids_id), timeout=15)
        return (resp.status_code == 200, resp.status_code)
    except requests.RequestException:
        return (False, -1)


def download_image(
    item: dict[str, Any], dest_dir: Path, filename_prefix: str = ""
) -> tuple[Path, str, int] | None:
    ids_id = get_ids_id(item)
    if not ids_id:
        return None
    safe_ids = "".join(c if c.isalnum() or c in "-_." else "_" for c in ids_id)
    name = f"{filename_prefix}{safe_ids}.jpg" if filename_prefix else f"{safe_ids}.jpg"
    dest = dest_dir / name
    url = iiif_image_url(ids_id)
    resp = SESSION.get(url, timeout=60)
    resp.raise_for_status()
    dest.write_bytes(resp.content)
    return dest, url, len(resp.content)


def jpeg_dimensions(path: Path) -> tuple[int, int] | None:
    """Stdlib-only JPEG dimension reader (parses SOF marker). Avoids Pillow."""
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


def first_with_iiif(items: Iterable[dict[str, Any]], n: int) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for it in items:
        if get_ids_id(it):
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
    report.append("# Smithsonian Open Access demo run\n\n")
    report.append(f"_Generated: {time.strftime('%Y-%m-%d %H:%M:%S %Z')}_\n\n")
    report.append(f"_API key in use: `{API_KEY_LABEL}`_\n\n")
    report.append(
        "This run exercises the four UBI features against the Smithsonian Open Access API "
        f"(`{SEARCH_URL}`): list collections (the 21 Smithsonian units), list artworks in a unit "
        "(range pagination via `start`+`rows`), keyword search, and IIIF download at "
        f"{PIXEL_SIZE}px on the longest side. Plus four Smithsonian-specific probes the firmware "
        "adapter needs answered: per-unit IIIF coverage, field-shape audit, rate-limit behavior, "
        "and a filter A/B for the production query.\n\n"
    )

    # ------------------------------------------------------------------
    # 1. List collections (units)
    # ------------------------------------------------------------------
    print("[1/4] Listing units (the Smithsonian's top-level collections)...")
    report.append("## 1. Collections (Smithsonian units)\n\n")
    report.append(
        "Smithsonian has 21 administrative \"units\" (sub-museums and archives). Each item carries "
        "a `unit_code` field, so units are the natural top-level grouping. Below is item count per "
        "unit, both unrestricted and restricted to `online_visual_material:true` (which is what the "
        "firmware adapter will query).\n\n"
    )

    art_counts: list[tuple[str, str, int, int]] = []  # (code, label, all, online)
    other_counts: list[tuple[str, str, int, int]] = []

    report.append("### Art-bearing units (p3a integration candidates)\n\n")
    report.append("| Unit code | Name | Total items | Online visual material |\n")
    report.append("|---|---|---:|---:|\n")
    for code, label in ART_UNITS:
        total = unit_count(code, online_only=False)
        online = unit_count(code, online_only=True)
        art_counts.append((code, label, total, online))
        print(f"      {code:8s} all={total:>9,}   online_visual={online:>9,}")
        report.append(f"| `{code}` | {label} | {total:,} | {online:,} |\n")
    report.append("\n")

    report.append("### Other units (for comparison; non-art, mostly excluded from integration)\n\n")
    report.append("| Unit code | Name | Total items | Online visual material |\n")
    report.append("|---|---|---:|---:|\n")
    for code, label in OTHER_UNITS:
        total = unit_count(code, online_only=False)
        online = unit_count(code, online_only=True)
        other_counts.append((code, label, total, online))
        print(f"      {code:8s} all={total:>9,}   online_visual={online:>9,}")
        report.append(f"| `{code}` | {label} | {total:,} | {online:,} |\n")
    report.append("\n")

    # ------------------------------------------------------------------
    # 2. Range pagination inside selected art units
    # ------------------------------------------------------------------
    print("[2/4] Range pagination inside art units...")
    report.append("## 2. Artworks in units (range pagination)\n\n")
    report.append(
        f"Demonstrating range-style pagination by requesting `start={PAGE_DEMO_OFFSET}, "
        f"rows={PAGE_DEMO_ROWS}` (i.e. items {PAGE_DEMO_OFFSET}–"
        f"{PAGE_DEMO_OFFSET + PAGE_DEMO_ROWS - 1}) inside each art-bearing unit.\n\n"
    )

    for code, label, _total, online in art_counts:
        # Some units may be smaller than PAGE_DEMO_OFFSET; in that case fall
        # back to start=0 so the demo still shows real data.
        if online >= PAGE_DEMO_OFFSET + PAGE_DEMO_ROWS:
            start = PAGE_DEMO_OFFSET
            note = ""
        else:
            start = 0
            note = (
                f" (unit has only {online:,} online items — falling back to "
                f"`start=0`)"
            )
        try:
            page = list_unit_items(code, start=start, rows=PAGE_DEMO_ROWS)
        except requests.HTTPError as exc:
            print(f"      [{code}] {label}: {exc}")
            report.append(f"### `{code}` — {label}\n\n_Request failed: {exc}_\n\n")
            continue
        items = _safe_get(page, "response", "rows") or []
        total = int(_safe_get(page, "response", "rowCount") or 0)
        print(f"      [{code}] {label}: total={total:,}, returned={len(items)}, start={start}")
        report.append(f"### `{code}` — {label}\n\n")
        report.append(
            f"Total in unit (with online media): **{total:,}** (filter: "
            f"`unit_code:{code} AND online_visual_material:true`).{note}\n\n"
        )
        if items:
            report.append("| Index | ID | Title | Artist | Date |\n|---:|---|---|---|---|\n")
            for i, it in enumerate(items):
                idx = start + i
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
    report.append(
        "Smithsonian's search uses Solr-style `q=`; the query string AND-concatenates the keyword "
        "with the `online_visual_material:true` filter.\n\n"
    )
    keyword_pages: dict[str, dict[str, Any]] = {}
    for kw in KEYWORDS:
        try:
            page = search_keyword(kw, start=0, rows=5)
        except requests.HTTPError as exc:
            print(f"      keyword '{kw}' failed: {exc}")
            report.append(f"### Keyword: `{kw}`\n\n_Request failed: {exc}_\n\n")
            continue
        items = _safe_get(page, "response", "rows") or []
        total = int(_safe_get(page, "response", "rowCount") or 0)
        keyword_pages[kw] = page
        print(f"      keyword '{kw}': total={total:,}, returned={len(items)}")
        report.append(f"### Keyword: `{kw}`\n\n")
        report.append(f"Total matches: **{total:,}**. First {len(items)}:\n\n")
        if items:
            report.append(
                "| # | ID | Title | Artist | Unit |\n|---:|---|---|---|---|\n"
            )
            for i, it in enumerate(items, start=1):
                report.append(
                    f"| {i} | `{it.get('id', '—')}` | {get_title(it)} | "
                    f"{get_artist(it)} | `{get_unit_label(it)}` |\n"
                )
            report.append("\n")
        else:
            report.append("_No items returned._\n\n")

    # ------------------------------------------------------------------
    # 4. Downloads (the primary deliverable for user QA)
    # ------------------------------------------------------------------
    print(f"[4/4] Downloading >= {DOWNLOADS_PER_UNIT} images per art unit at 720px...")
    report.append("## 4. Downloads\n\n")
    report.append(
        f"At least **{DOWNLOADS_PER_UNIT} images per art-bearing unit** are saved to "
        f"`output/images/`, plus {DOWNLOADS_PER_UNIT} per keyword. All images requested via IIIF "
        f"Image API at `{IIIF_PREFIX}<idsId>/full/!{PIXEL_SIZE},{PIXEL_SIZE}/0/default.jpg` (fit "
        f"within {PIXEL_SIZE}×{PIXEL_SIZE}, aspect preserved → longest side is exactly "
        f"{PIXEL_SIZE}).\n\n"
        "Filename convention: `{unit_code}_{idsId}.jpg` for unit downloads, "
        "`search_{kw}_{idsId}.jpg` for keyword downloads. The user reviews these files visually "
        "at the Phase A checkpoint.\n\n"
    )

    download_log: list[dict[str, Any]] = []

    def fetch_targets(group_label: str, items: list[dict[str, Any]], prefix: str) -> None:
        targets = first_with_iiif(items, DOWNLOADS_PER_UNIT)
        for it in targets:
            obj = it.get("id") or "unknown"
            print(f"      downloading [{group_label}] {obj}")
            try:
                result = download_image(it, IMAGES_DIR, filename_prefix=prefix)
            except Exception as exc:
                download_log.append({
                    "group": group_label, "object": obj,
                    "title": get_title(it), "artist": get_artist(it),
                    "rights": get_rights(it),
                    "error": f"{type(exc).__name__}: {exc}",
                })
                continue
            if result is None:
                download_log.append({
                    "group": group_label, "object": obj,
                    "title": get_title(it), "artist": get_artist(it),
                    "rights": get_rights(it),
                    "error": "no idsId in record",
                })
                continue
            dest, url, size = result
            dims = jpeg_dimensions(dest)
            download_log.append({
                "group": group_label, "object": obj,
                "title": get_title(it), "artist": get_artist(it),
                "rights": get_rights(it),
                "file": dest.name, "bytes": size, "dims": dims, "url": url,
            })

    # Per-unit downloads
    for code, label, _total, _online in art_counts:
        try:
            # Pull a wider window than DOWNLOADS_PER_UNIT to give first_with_iiif
            # room to skip records that lack idsId.
            page = list_unit_items(code, start=0, rows=20)
        except requests.HTTPError as exc:
            print(f"      unit {code}: listing failed: {exc}")
            continue
        items = _safe_get(page, "response", "rows") or []
        fetch_targets(f"unit: {code}", items, prefix=f"{code}_")

    # Keyword downloads
    for kw in KEYWORDS:
        try:
            page = search_keyword(kw, start=0, rows=20)
        except requests.HTTPError as exc:
            print(f"      search '{kw}': {exc}")
            continue
        items = _safe_get(page, "response", "rows") or []
        fetch_targets(f"search: {kw}", items, prefix=f"search_{kw}_")

    if download_log:
        report.append(
            "| Group | ID | Title | Artist | Rights | File | Dimensions | Bytes | URL |\n"
            "|---|---|---|---|---|---|---|---:|---|\n"
        )
        for entry in download_log:
            file_cell = entry.get("file", "—")
            dims = entry.get("dims")
            dims_cell = f"{dims[0]}×{dims[1]}" if dims else (entry.get("error") or "—")
            bytes_cell = f"{entry.get('bytes', 0):,}" if "bytes" in entry else "—"
            url_cell = entry.get("url", "—")
            report.append(
                f"| {entry['group']} | `{entry['object']}` | {entry['title']} | "
                f"{entry['artist']} | {entry.get('rights', '—')} | `{file_cell}` | "
                f"{dims_cell} | {bytes_cell} | {url_cell} |\n"
            )
        report.append("\n")

    # ------------------------------------------------------------------
    # Smithsonian-specific probe A: per-unit IIIF coverage
    # ------------------------------------------------------------------
    print(f"[probe A] Per-unit IIIF coverage (sample {COVERAGE_SAMPLE_SIZE}, info.json probe {COVERAGE_IIIF_PROBE_SIZE})...")
    report.append("## A. Per-unit IIIF coverage\n\n")
    report.append(
        f"For each art-bearing unit, sample the first {COVERAGE_SAMPLE_SIZE} hits of "
        f"`unit_code:{{X}} AND online_visual_material:true` and report:\n\n"
        f"- **idsId present**: fraction of hits whose JSON contains a usable `idsId`.\n"
        f"- **info.json OK**: of the first {COVERAGE_IIIF_PROBE_SIZE} with `idsId`, fraction "
        f"whose `info.json` returns HTTP 200 (confirming IIIF actually serves the image).\n\n"
        "Units with **< 80% on both** are weak integration candidates.\n\n"
        "| Unit | Sampled | idsId present | info.json OK (of probed) |\n"
        "|---|---:|---:|---:|\n"
    )
    coverage_summary: list[tuple[str, int, int, int, int]] = []
    for code, label, _total, _online in art_counts:
        try:
            page = list_unit_items(code, start=0, rows=COVERAGE_SAMPLE_SIZE)
        except requests.HTTPError as exc:
            print(f"      [{code}] sample failed: {exc}")
            report.append(f"| `{code}` | — | — | _request failed: {exc}_ |\n")
            continue
        items = _safe_get(page, "response", "rows") or []
        with_ids = [it for it in items if get_ids_id(it)]
        probe_set = with_ids[:COVERAGE_IIIF_PROBE_SIZE]
        info_ok = 0
        for it in probe_set:
            ok, _status = probe_iiif_info(get_ids_id(it) or "")
            if ok:
                info_ok += 1
        coverage_summary.append((code, len(items), len(with_ids), len(probe_set), info_ok))
        pct_ids = (len(with_ids) / len(items) * 100) if items else 0
        pct_iiif = (info_ok / len(probe_set) * 100) if probe_set else 0
        print(f"      {code:8s} sampled={len(items)}  idsId={len(with_ids)}/{len(items)} ({pct_ids:.0f}%)  info.json={info_ok}/{len(probe_set)} ({pct_iiif:.0f}%)")
        report.append(
            f"| `{code}` | {len(items)} | {len(with_ids)}/{len(items)} "
            f"({pct_ids:.0f}%) | {info_ok}/{len(probe_set)} ({pct_iiif:.0f}%) |\n"
        )
    report.append("\n")

    # ------------------------------------------------------------------
    # Smithsonian-specific probe B: field-shape audit
    # ------------------------------------------------------------------
    print("[probe B] Field-shape audit on one SAAM record...")
    report.append("## B. Field-shape audit\n\n")
    report.append(
        "Concrete JSON paths the firmware C parser will hardcode. Sample taken from the first "
        "SAAM record with `idsId`. If the paths below change in the future, the C adapter must "
        "be updated correspondingly.\n\n"
    )
    try:
        sample_page = list_unit_items("SAAM", start=0, rows=20)
        sample_items = _safe_get(sample_page, "response", "rows") or []
        sample = next((it for it in sample_items if get_ids_id(it)), None)
    except requests.HTTPError as exc:
        sample = None
        report.append(f"_Sample fetch failed: {exc}_\n\n")
    if sample is not None:
        ids_id = get_ids_id(sample) or "(none)"
        report.append(
            "| Field | JSON path | Sample value |\n|---|---|---|\n"
            f"| Object ID | `id` | `{sample.get('id', '—')}` |\n"
            f"| Title | `title` | {get_title(sample)} |\n"
            f"| Artist | `content.freetext.name[*].content` (where label ∈ artist/maker/creator) | {get_artist(sample)} |\n"
            f"| Date | `content.freetext.date[*].content` | {get_date(sample)} |\n"
            f"| Rights | `content.freetext.objectRights[*].content` | {get_rights(sample)} |\n"
            f"| Unit code | `content.descriptiveNonRepeating.unit_code` | `{get_unit_label(sample)}` |\n"
            f"| IIIF ID | `content.descriptiveNonRepeating.online_media.media[*].idsId` | `{ids_id}` |\n"
        )
        report.append("\n")
        report.append(
            f"Constructed IIIF URL example: `{iiif_image_url(ids_id)}`\n\n"
            f"Constructed info.json example: `{iiif_info_url(ids_id)}`\n\n"
        )
        report.append(
            "Note: the `online_media.media` field can be either a single object or a list of "
            "objects depending on how many media files the record has. The C parser must handle "
            "both shapes (mirror what `get_ids_id()` in this script does).\n\n"
        )

    # ------------------------------------------------------------------
    # Smithsonian-specific probe C: rate-limit behavior
    # ------------------------------------------------------------------
    print(f"[probe C] Rate-limit burst ({RATE_LIMIT_BURST} rapid requests)...")
    report.append("## C. Rate-limit behavior\n\n")
    report.append(
        f"Sends {RATE_LIMIT_BURST} rapid `rows=0` requests against the search endpoint and "
        f"records status codes + any `Retry-After` headers. Captures the actual 429 wire format "
        f"so the firmware adapter knows what to parse.\n\n"
        f"API key in use for this probe: `{API_KEY_LABEL}`. The api.data.gov default for DEMO_KEY is "
        "30 requests/hour/IP (intentionally low); a registered key bumps to 1,000/hour by default.\n\n"
    )
    statuses: list[tuple[int, int, str]] = []  # (i, status, retry_after)
    for i in range(RATE_LIMIT_BURST):
        try:
            resp = SESSION.get(
                SEARCH_URL,
                params={"api_key": API_KEY, "q": "unit_code:SAAM", "rows": 0},
                timeout=15,
            )
            retry_after = resp.headers.get("Retry-After", "")
            statuses.append((i + 1, resp.status_code, retry_after))
            if resp.status_code == 429:
                # Stop early once we've captured the throttle response.
                print(f"      hit 429 at request #{i + 1}, Retry-After={retry_after!r}; stopping burst")
                break
        except requests.RequestException as exc:
            statuses.append((i + 1, -1, str(exc)))
            break
    report.append("| Request # | Status | Retry-After |\n|---:|---:|---|\n")
    for n, status, ra in statuses:
        ra_cell = f"`{ra}`" if ra else "—"
        report.append(f"| {n} | {status} | {ra_cell} |\n")
    report.append("\n")
    saw_429 = any(s == 429 for _, s, _ in statuses)
    if saw_429:
        first_429 = next(n for n, s, _ in statuses if s == 429)
        report.append(
            f"Throttle observed at request **#{first_429}**. Firmware adapter must parse "
            "`Retry-After` (if present) and engage the existing per-museum cooldown via "
            "`art_institution_set_rate_limited(\"si\", cooldown_sec)`.\n\n"
        )
    else:
        report.append(
            f"No 429 within {RATE_LIMIT_BURST} requests — firmware will still need defensive "
            "429 handling (it's normal for a real BYOK key under heavy refresh load).\n\n"
        )

    # ------------------------------------------------------------------
    # Smithsonian-specific probe D: filter A/B
    # ------------------------------------------------------------------
    print("[probe D] Filter A/B for production query...")
    report.append("## D. Filter A/B — selecting the production query\n\n")
    report.append(
        "Counts for one art-bearing unit (SAAM) under three filter strategies. Picks the most "
        "restrictive filter that still yields enough content per unit for a healthy channel.\n\n"
        "| Filter | SAAM count |\n|---|---:|\n"
    )
    filters_ab = [
        ("unit_code:SAAM", "unit_code:SAAM"),
        (
            "unit_code:SAAM AND online_visual_material:true",
            "unit_code:SAAM AND online_visual_material:true",
        ),
        (
            "unit_code:SAAM AND online_visual_material:true AND usage:CC0",
            "unit_code:SAAM AND online_visual_material:true AND usage:CC0",
        ),
    ]
    ab_results: list[tuple[str, int]] = []
    for label, q in filters_ab:
        try:
            c = _count_for_query(q)
        except requests.HTTPError as exc:
            c = -1
            print(f"      filter '{label}' failed: {exc}")
        ab_results.append((label, c))
        cell = f"{c:,}" if c >= 0 else "_request failed_"
        report.append(f"| `{label}` | {cell} |\n")
    report.append("\n")
    report.append(
        "**Recommended production filter** is the strictest one that retains enough content "
        "(threshold: ~1,000 items per unit for a healthy channel). Firmware default: "
        "`unit_code:{X} AND online_visual_material:true` unless probe D shows that the explicit "
        "`AND usage:CC0` clause yields comparable counts (in which case prefer it for rights "
        "clarity).\n\n"
    )

    # ------------------------------------------------------------------
    # Deep pagination probe (informs firmware channel_offset clamping)
    # ------------------------------------------------------------------
    print(f"[probe E] Deep pagination probe at offsets {DEEP_PAGE_OFFSETS}...")
    report.append("## E. Deep pagination probe\n\n")
    report.append(
        "Tests `start` values from 0 up to 10,000 on `unit_code:CHNDM AND online_visual_material:true` "
        "(CHNDM has the most online visual items among art units — 54K+, so it's the only one big "
        "enough to exercise deep pagination). Confirms whether the API caps deep offsets the way "
        "AIC does at 10K.\n\n"
        "| start | rowCount | rows returned | first ID |\n|---:|---:|---:|---|\n"
    )
    for start in DEEP_PAGE_OFFSETS:
        try:
            page = search(
                q="unit_code:CHNDM AND online_visual_material:true",
                start=start,
                rows=3,
            )
            row_count = int(_safe_get(page, "response", "rowCount") or 0)
            rows = _safe_get(page, "response", "rows") or []
            first_id = rows[0].get("id", "—") if rows else "—"
            report.append(f"| {start:,} | {row_count:,} | {len(rows)} | `{first_id}` |\n")
            print(f"      start={start:>6,}  rowCount={row_count:>9,}  returned={len(rows)}")
        except requests.HTTPError as exc:
            report.append(f"| {start:,} | — | — | _{exc}_ |\n")
            print(f"      start={start:>6,}  failed: {exc}")
    report.append("\n")

    # ------------------------------------------------------------------
    # Run summary
    # ------------------------------------------------------------------
    successes = [e for e in download_log if "bytes" in e]
    correct_size = [e for e in successes if e.get("dims") and PIXEL_SIZE in e["dims"]]
    cc0_count = sum(1 for e in successes if "CC0" in str(e.get("rights", "")).upper())
    elapsed = time.time() - started

    report.append("## Run summary\n\n")
    report.append(f"- Art-bearing units surveyed: **{len(art_counts)}**\n")
    report.append(f"- Other units surveyed (comparison): **{len(other_counts)}**\n")
    report.append(f"- Keyword searches: **{len(KEYWORDS)}** ({', '.join(KEYWORDS)})\n")
    report.append(
        f"- Downloads attempted: **{len(download_log)}** "
        f"(succeeded: **{len(successes)}**, with longest-side=={PIXEL_SIZE}: **{len(correct_size)}**, "
        f"CC0-tagged: **{cc0_count}**)\n"
    )
    report.append(f"- Coverage rows recorded: **{len(coverage_summary)}**\n")
    report.append(f"- Rate-limit burst: **{len(statuses)}** requests, "
                  f"throttle {'observed' if saw_429 else 'not observed'}\n")
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
