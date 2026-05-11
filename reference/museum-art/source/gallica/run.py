"""BnF Gallica integration demo.

Exercises the four UBI capabilities against Gallica's SRU search API and
IIIF Image API v1.1:

  1. List collections — Gallica's `dc.type` enum (image, manuscrit,
     carte, monographie, periodique, partition). Counts are probed live.
  2. List artworks inside a collection (range-style pagination via
     ``startRecord`` + ``maximumRecords`` — SRU is 1-based).
  3. List artworks via keyword search (``dc.title all "<kw>"``).
  4. Download artworks at exactly 720px on the longest side via IIIF
     Image API v1.1.

Writes a Markdown report to ``output/report.md`` and the downloaded JPEGs
to ``output/images/``.

API references:
- SRU: https://api.bnf.fr/api-sru-de-gallica
- IIIF Image v1.1 (level 2): https://api.bnf.fr/api-iiif-de-recuperation-des-images-de-gallica
- IIIF Presentation v2: ``https://gallica.bnf.fr/iiif/ark:/12148/{ark}/manifest.json``

Notes / quirks:
- The SRU endpoint returns ``HTTP 403`` to bare ``python-requests`` /
  ``curl`` user agents, so this script sends a browser-like UA.
- A "document" in Gallica is a single ARK that may contain many folios
  (``f1``, ``f2``, …). For the demo each document is treated as one
  "artwork" and we always download its first folio (``f1``).
- IIIF Image v1.1 quality is ``native.jpg`` (the v1.1 spec value);
  ``default.jpg`` is also accepted for compatibility. The ``!w,h``
  best-fit size syntax is in v1.1 too, so the same call shape used
  for the v2 demos works here.
- Each Gallica document spans many fields; the SRU response is
  Dublin-Core XML (OAI-PMH style), not JSON.

Requires: requests
"""

from __future__ import annotations

import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any, Iterable

import requests

SRU_URL = "https://gallica.bnf.fr/SRU"
IIIF_BASE = "https://gallica.bnf.fr/iiif"

SCRIPT_DIR = Path(__file__).resolve().parent
OUTPUT_DIR = SCRIPT_DIR / "output"
IMAGES_DIR = OUTPUT_DIR / "images"
REPORT_PATH = OUTPUT_DIR / "report.md"

PIXEL_SIZE = 720  # longest side, aspect preserved

# Gallica's `dc.type` values that map cleanly to IIIF documents and so are
# usable as "collections" for the UBI. The SRU value is the case-folded
# French term that BnF's index uses; the display label is what we show.
COLLECTIONS: tuple[tuple[str, str], ...] = (
    ("image",        "Images (prints, drawings, photographs)"),
    ("manuscrit",    "Manuscripts"),
    ("carte",        "Maps"),
    ("monographie",  "Monographs (books)"),
    ("periodique",   "Periodicals"),
    ("partition",    "Sheet music"),
)

KEYWORDS = ["landscape", "horse"]
PAGE_DEMO_OFFSET = 200
PAGE_DEMO_ROWS = 5
DOWNLOADS_PER_GROUP = 3

# SRU is 1-based: offset 0 == startRecord 1.
def _to_start_record(offset: int) -> int:
    return offset + 1


# Namespaces in the SRU response.
NS = {
    "srw":  "http://www.loc.gov/zing/srw/",
    "diag": "http://www.loc.gov/zing/srw/diagnostic/",
    "oai":  "http://www.openarchives.org/OAI/2.0/oai_dc/",
    "dc":   "http://purl.org/dc/elements/1.1/",
}

SESSION = requests.Session()
SESSION.headers.update({
    # Gallica returns 403 to plain curl/requests fingerprints, so we send a
    # browser-like UA. A custom suffix preserves attribution.
    "User-Agent": (
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36 "
        "museum-art/0.1 (+Gallica demo)"
    ),
    "Accept": "application/xml,text/xml;q=0.9,*/*;q=0.8",
})


# ---------------------------------------------------------------------------
# SRU helpers
# ---------------------------------------------------------------------------


def sru_query(query: str, start_record: int = 1, max_records: int = 10) -> ET.Element:
    """Run an SRU searchRetrieve and return the parsed root element.

    Surfaces SRU diagnostics as RuntimeError so the caller doesn't have to
    re-parse a quietly-empty response.
    """
    params = {
        "version": "1.2",
        "operation": "searchRetrieve",
        "query": query,
        "startRecord": start_record,
        "maximumRecords": max_records,
    }
    response = SESSION.get(SRU_URL, params=params, timeout=30)
    response.raise_for_status()
    root = ET.fromstring(response.content)
    diag = root.find(".//diag:details", NS)
    if diag is not None and (diag.text or "").strip():
        raise RuntimeError(f"SRU diagnostic: {diag.text!r} for query {query!r}")
    return root


def total_for(query: str) -> int:
    """Cheap count probe: ask for one record and read the totalled count."""
    root = sru_query(query, start_record=1, max_records=1)
    n = root.find("srw:numberOfRecords", NS)
    return int((n.text or "0").strip()) if n is not None else 0


def collection_query(coll_value: str) -> str:
    """SRU CQL for one of our `dc.type` collections."""
    return f'dc.type all "{coll_value}"'


def keyword_query(kw: str) -> str:
    """SRU CQL for a keyword search. ``dc.title`` is the closest analogue
    to the keyword search semantics in our other demos. ``bib.any`` and
    ``gallica.contenu`` are *not* valid SRU indexes on Gallica.
    """
    return f'dc.title all "{kw}"'


# ---------------------------------------------------------------------------
# Record extraction
# ---------------------------------------------------------------------------


def iter_records(root: ET.Element) -> Iterable[dict[str, Any]]:
    """Yield one normalized dict per ``<srw:record>`` in the SRU response."""
    for rec in root.findall(".//srw:record", NS):
        rd = rec.find("srw:recordData", NS)
        if rd is None:
            continue
        out: dict[str, Any] = {"creators": [], "titles": [], "dates": [], "types": [], "ark": None}
        for child in rd.iter():
            tag = child.tag.split("}")[-1]
            text = (child.text or "").strip()
            if not text:
                continue
            if tag == "title":
                out["titles"].append(text)
            elif tag == "creator":
                out["creators"].append(text)
            elif tag == "date":
                out["dates"].append(text)
            elif tag == "type":
                out["types"].append(text)
            elif tag == "identifier" and text.startswith("https://gallica.bnf.fr/ark:/"):
                # e.g. https://gallica.bnf.fr/ark:/12148/btv1b7700420q
                out["ark"] = text.split("ark:/", 1)[1]  # "12148/btv1b7700420q"
        yield out


def get_title(rec: dict[str, Any]) -> str:
    titles = rec.get("titles") or []
    title = titles[0] if titles else "(untitled)"
    if len(title) > 140:
        title = title[:137] + "..."
    return title


def get_artist(rec: dict[str, Any]) -> str:
    creators = rec.get("creators") or []
    return creators[0] if creators else "(unknown)"


def get_date(rec: dict[str, Any]) -> str:
    dates = rec.get("dates") or []
    return dates[0] if dates else "—"


# ---------------------------------------------------------------------------
# IIIF v1.1 image download + JPEG dimension probe
# ---------------------------------------------------------------------------


def iiif_image_url(ark: str, max_side: int = PIXEL_SIZE) -> str:
    """IIIF Image API v1.1 URL for the first folio of an ARK at best-fit size."""
    # ark is of the form "12148/btv1b7700420q"; we always pull folio 1.
    return f"{IIIF_BASE}/ark:/{ark}/f1/full/!{max_side},{max_side}/0/native.jpg"


def download_image(rec: dict[str, Any], dest_dir: Path) -> tuple[Path, str, int] | None:
    ark = rec.get("ark")
    if not ark:
        return None
    safe_name = "".join(c if c.isalnum() or c in "-_." else "_" for c in str(ark))
    dest = dest_dir / f"{safe_name}.jpg"
    url = iiif_image_url(ark)
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


def first_with_ark(items: Iterable[dict[str, Any]], n: int) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for it in items:
        if it.get("ark"):
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
    report.append("# BnF Gallica demo run\n\n")
    report.append(f"_Generated: {time.strftime('%Y-%m-%d %H:%M:%S %Z')}_\n\n")
    report.append(
        "This run exercises the four UBI features against BnF Gallica: list "
        "collections (`dc.type` enum), list artworks in a collection (range-"
        "style pagination via `startRecord`+`maximumRecords` — SRU is 1-based), "
        f"keyword search (`dc.title all`), and IIIF Image v1.1 download at "
        f"{PIXEL_SIZE}px on the longest side.\n\n"
        f"All requests go through `{SRU_URL}`. Gallica returns HTTP 403 to "
        "default `requests` / `curl` fingerprints, so the script sends a "
        "browser-like User-Agent.\n\n"
    )

    # ------------------------------------------------------------------
    # 1. List collections (dc.type counts)
    # ------------------------------------------------------------------
    print("[1/4] Listing collections (`dc.type` enum)...")
    report.append("## 1. Collections\n\n")
    report.append(
        "Gallica is a digital library, not a museum, so its top-level "
        "grouping is the **`dc.type`** enum (Dublin Core type vocabulary). "
        "We use the six values that map cleanly to IIIF documents. SRU "
        "doesn't expose facet counts directly, so each row here is the "
        "result of a 1-record `numberOfRecords` probe.\n\n"
    )

    coll_counts: list[tuple[str, str, int]] = []  # (value, label, count)
    for value, label in COLLECTIONS:
        try:
            count = total_for(collection_query(value))
        except (requests.HTTPError, RuntimeError) as exc:
            print(f"  collection '{value}' failed: {exc}")
            count = 0
        coll_counts.append((value, label, count))
        print(f"      {value:<14} {label}: {count:,}")

    if coll_counts:
        report.append("| `dc.type` value | Label | Count |\n|---|---|---:|\n")
        for value, label, count in coll_counts:
            report.append(f"| `{value}` | {label} | {count:,} |\n")
        report.append("\n")

    # Pick the two largest collections that have at least
    # PAGE_DEMO_OFFSET + PAGE_DEMO_ROWS records, for downloads. Show range
    # pagination on all of them with the offset==0 fallback if needed.
    coll_counts.sort(key=lambda r: -r[2])
    print(f"      sorted by size: {[(v, c) for v, _, c in coll_counts]}")

    # ------------------------------------------------------------------
    # 2. Range pagination inside collections
    # ------------------------------------------------------------------
    print("[2/4] Listing artworks inside collections (range pagination)...")
    report.append("## 2. Artworks in collections (range pagination)\n\n")
    report.append(
        f"Demonstrating range-style pagination by requesting `offset={PAGE_DEMO_OFFSET}, "
        f"rows={PAGE_DEMO_ROWS}` (i.e. records {PAGE_DEMO_OFFSET}–"
        f"{PAGE_DEMO_OFFSET + PAGE_DEMO_ROWS - 1}). SRU is 1-based, so the "
        f"adapter computes "
        f"`startRecord = offset+1 = {PAGE_DEMO_OFFSET + 1}` "
        f"and `maximumRecords = {PAGE_DEMO_ROWS}`.\n\n"
    )

    for value, label, count in coll_counts:
        if count == 0:
            continue
        if count >= PAGE_DEMO_OFFSET + PAGE_DEMO_ROWS:
            offset = PAGE_DEMO_OFFSET
            note = ""
        else:
            offset = 0
            note = (
                f" (collection has only {count:,} records — falling back to "
                f"`offset=0` instead of {PAGE_DEMO_OFFSET})"
            )
        try:
            root = sru_query(
                collection_query(value),
                start_record=_to_start_record(offset),
                max_records=PAGE_DEMO_ROWS,
            )
        except (requests.HTTPError, RuntimeError) as exc:
            print(f"      [{value}] {label}: {exc}")
            report.append(f"### `{value}`: {label}\n\n")
            report.append(f"_Request failed: {exc}_\n\n")
            continue
        items = list(iter_records(root))
        print(f"      [{value}] {label}: total={count:,}, returned={len(items)}, offset={offset}")
        report.append(f"### `{value}`: {label}\n\n")
        report.append(f"Total in this bucket: **{count:,}** (filter: `dc.type all \"{value}\"`).{note}\n\n")
        if items:
            report.append("| Index | ARK | Title | Creator | Date |\n|---:|---|---|---|---|\n")
            for i, it in enumerate(items):
                idx = offset + i
                ark = it.get("ark") or "—"
                report.append(
                    f"| {idx} | `{ark}` | {get_title(it)} | "
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
        "Search uses `dc.title all \"<kw>\"`. (`gallica.contenu` and "
        "`bib.any` are advertised in some third-party docs but BnF's SRU "
        "rejects them with `There are no translation for the following key`.)\n\n"
    )

    keyword_records: dict[str, list[dict[str, Any]]] = {}
    for kw in KEYWORDS:
        try:
            root = sru_query(keyword_query(kw), start_record=1, max_records=5)
        except (requests.HTTPError, RuntimeError) as exc:
            print(f"      keyword '{kw}' failed: {exc}")
            report.append(f"### Keyword: `{kw}`\n\n_Request failed: {exc}_\n\n")
            continue
        items = list(iter_records(root))
        n = root.find("srw:numberOfRecords", NS)
        total = int((n.text or "0").strip()) if n is not None else 0
        keyword_records[kw] = items
        print(f"      keyword '{kw}': total={total:,}, returned={len(items)}")
        report.append(f"### Keyword: `{kw}`\n\n")
        report.append(f"Total matches: **{total:,}**. First {len(items)}:\n\n")
        if items:
            report.append("| # | ARK | Title | Creator | Date |\n|---:|---|---|---|---|\n")
            for i, it in enumerate(items, start=1):
                ark = it.get("ark") or "—"
                report.append(
                    f"| {i} | `{ark}` | {get_title(it)} | "
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
        f"**2 keyword searches**. All images requested via the IIIF Image "
        f"API v1.1 at "
        f"`{IIIF_BASE}/ark:/<ark>/f1/full/!{PIXEL_SIZE},{PIXEL_SIZE}/0/native.jpg` "
        f"(fit within {PIXEL_SIZE}×{PIXEL_SIZE}, aspect preserved → longest "
        f"side is exactly {PIXEL_SIZE}). Multi-folio Gallica documents "
        "(manuscripts, books, periodicals) collapse to **folio 1** for the "
        "demo — typically the title page or cover.\n\n"
        "Files are saved under `output/images/`.\n\n"
    )

    download_log: list[dict[str, Any]] = []

    def fetch_targets(group_label: str, items: list[dict[str, Any]]) -> None:
        targets = first_with_ark(items, DOWNLOADS_PER_GROUP)
        for it in targets:
            ark = it.get("ark") or "unknown"
            print(f"      downloading [{group_label}] {ark}")
            try:
                result = download_image(it, IMAGES_DIR)
            except Exception as exc:
                download_log.append({
                    "group": group_label, "object": ark,
                    "title": get_title(it), "artist": get_artist(it),
                    "error": f"{type(exc).__name__}: {exc}",
                })
                continue
            if result is None:
                download_log.append({
                    "group": group_label, "object": ark,
                    "title": get_title(it), "artist": get_artist(it),
                    "error": "no ARK identifier",
                })
                continue
            dest, url, size = result
            dims = jpeg_dimensions(dest)
            download_log.append({
                "group": group_label, "object": ark,
                "title": get_title(it), "artist": get_artist(it),
                "file": dest.name, "bytes": size, "dims": dims, "url": url,
            })

    download_collections = [(v, l, c) for v, l, c in coll_counts if c > 0][:2]
    for value, label, _count in download_collections:
        try:
            root = sru_query(collection_query(value), start_record=1, max_records=20)
        except (requests.HTTPError, RuntimeError) as exc:
            print(f"      collection [{value}] {label}: {exc}")
            continue
        fetch_targets(f"[{value}] {label}", list(iter_records(root)))

    for kw in KEYWORDS:
        try:
            root = sru_query(keyword_query(kw), start_record=1, max_records=20)
        except (requests.HTTPError, RuntimeError) as exc:
            print(f"      search '{kw}': {exc}")
            continue
        fetch_targets(f"search: {kw}", list(iter_records(root)))

    if download_log:
        report.append(
            "| Group | ARK | Title | Creator | File | Dimensions | Bytes | URL |\n"
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
        f"- Collections probed: **{len(COLLECTIONS)}** "
        f"({', '.join(f'`{v}`' for v, _ in COLLECTIONS)})\n"
    )
    report.append(
        f"- Collections used for downloads: **{len(download_collections)}** "
        f"({', '.join(f'[{v}] {l}' for v, l, _ in download_collections)})\n"
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
