"""Probe the Rijksmuseum data services and IIIF stack.

Exercises the three UBI features (list collections, list artworks in a
collection, keyword search) plus IIIF image download at a fixed 720px
longest-side, and writes a Markdown report of what it found.

Sources used (all anonymous, no API key):
  - OAI-PMH ListSets (XML)         to enumerate curated Sets
  - Linked Art search/collection   to list artworks in a Set + cursor pagination
  - Linked Art search/collection   to approximate keyword search via title/description fan-out
  - Linked Art HMO -> VisualItem -> DigitalObject chain to discover IIIF Image URLs
  - IIIF Image API on iiif.micr.io to download at /full/!720,720/0/default.jpg
"""
from __future__ import annotations

import io
import re
import sys
import time
import urllib.parse
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path

import requests

BASE = "https://data.rijksmuseum.nl"
OAI = f"{BASE}/oai"
SEARCH = f"{BASE}/search/collection"
ID_PREFIX = "https://id.rijksmuseum.nl/"
USER_AGENT = "museum-art-explore/0.1 (+https://github.com/; pub@kury.dev)"

OUT_DIR = Path(__file__).parent / "out"
IMG_DIR = OUT_DIR / "images"
REPORT_PATH = OUT_DIR / "REPORT.md"

DOWNLOADS_PER_BUCKET = 3
KEYWORDS = ["Vermeer", "tulip"]

session = requests.Session()
session.headers.update({"User-Agent": USER_AGENT})


def get_jsonld(url: str) -> dict:
    r = session.get(url, headers={"Accept": "application/ld+json"}, timeout=30)
    r.raise_for_status()
    return r.json()


# ---------------------------------------------------------------------------
# 1. Collections — OAI-PMH ListSets

@dataclass
class SetInfo:
    spec: str
    name: str


def list_sets() -> list[SetInfo]:
    r = session.get(f"{OAI}?verb=ListSets", timeout=30)
    r.raise_for_status()
    ns = {"oai": "http://www.openarchives.org/OAI/2.0/"}
    root = ET.fromstring(r.content)
    out: list[SetInfo] = []
    for s in root.findall(".//oai:set", ns):
        spec = (s.findtext("oai:setSpec", namespaces=ns) or "").strip()
        name = (s.findtext("oai:setName", namespaces=ns) or "").strip()
        if spec and name:
            out.append(SetInfo(spec, name))
    return out


def pick_demo_sets(sets: list[SetInfo]) -> list[SetInfo]:
    """Pick two well-known English-named painting Sets for the demo."""
    preferred_specs = {"26121", "26118"}  # Dutch 17C Paintings, Flemish Paintings
    by_spec = {s.spec: s for s in sets}
    chosen = [by_spec[s] for s in preferred_specs if s in by_spec]
    if len(chosen) >= 2:
        return chosen[:2]
    # Fallback: first two sets whose name contains "painting" / "schilderij"
    fallback = [s for s in sets if re.search(r"paint|schilder", s.name, re.I)]
    if len(fallback) >= 2:
        return fallback[:2]
    return sets[:2]


# ---------------------------------------------------------------------------
# 2. Artworks in a Set — search/collection?memberOfSetId=…

@dataclass
class PageWalk:
    items: list[dict]
    total: int | None
    pages_fetched: int


def walk_collection(url: str, max_items: int) -> PageWalk:
    items: list[dict] = []
    pages = 0
    total: int | None = None
    while url and len(items) < max_items:
        d = get_jsonld(url)
        if total is None and isinstance(d.get("partOf"), dict):
            total = d["partOf"].get("totalItems")
        items.extend(d.get("orderedItems", []) or [])
        pages += 1
        nxt = d.get("next")
        url = nxt["id"] if isinstance(nxt, dict) else None
    return PageWalk(items[:max_items], total, pages)


def list_set_artworks(set_spec: str, max_items: int = 10) -> PageWalk:
    member_id = f"{ID_PREFIX}{set_spec}"
    url = (
        f"{SEARCH}?memberOfSetId={urllib.parse.quote(member_id, safe='')}"
        f"&imageAvailable=true"
    )
    return walk_collection(url, max_items=max_items)


# ---------------------------------------------------------------------------
# 3. Keyword search — fan out across title + description, union client-side.
# The new data.rijksmuseum.nl API has no free-text `q` parameter; only scoped
# fields are accepted. This is an emulation of "keyword search" and is
# documented as such in the report.

@dataclass
class KeywordSearch:
    keyword: str
    field_totals: dict[str, int | None]
    union_items: list[dict]


def keyword_search(keyword: str, fields=("title", "description"), per_field: int = 25) -> KeywordSearch:
    seen: dict[str, dict] = {}
    totals: dict[str, int | None] = {}
    for f in fields:
        url = f"{SEARCH}?{f}={urllib.parse.quote(keyword)}&imageAvailable=true"
        d = get_jsonld(url)
        totals[f] = d.get("partOf", {}).get("totalItems") if isinstance(d.get("partOf"), dict) else None
        for it in (d.get("orderedItems") or [])[:per_field]:
            seen.setdefault(it["id"], it)
    return KeywordSearch(keyword, totals, list(seen.values()))


# ---------------------------------------------------------------------------
# 4. IIIF discovery + 720px download
# Linked Art chain: HumanMadeObject.shows[] -> VisualItem.digitally_shown_by[]
# -> DigitalObject.access_point[]. The access_point URL is a full IIIF image
# request like https://iiif.micr.io/{id}/full/max/0/default.jpg ; we extract
# the image-service base and re-request at /full/!720,720/0/default.jpg.

MICRIO_RE = re.compile(r"^(https://iiif\.micr\.io/[^/]+)")


@dataclass
class IIIFRef:
    iiif_base: str
    micrio_id: str
    title: str
    object_number: str | None


def _first_name(hmo: dict) -> str:
    for n in hmo.get("identified_by", []) or []:
        if n.get("type") == "Name" and n.get("content"):
            return n["content"]
    return "(untitled)"


def _object_number(hmo: dict) -> str | None:
    for n in hmo.get("identified_by", []) or []:
        if n.get("type") == "Identifier" and n.get("content"):
            for cls in n.get("classified_as", []) or []:
                if "300312355" in cls.get("id", ""):  # AAT: accession numbers
                    return n["content"]
    return None


def find_iiif(hmo_id: str) -> IIIFRef | None:
    hmo = get_jsonld(hmo_id)
    title = _first_name(hmo)
    obj_num = _object_number(hmo)
    for vi_ref in hmo.get("shows", []) or []:
        try:
            visual = get_jsonld(vi_ref["id"])
        except requests.RequestException:
            continue
        for do_ref in visual.get("digitally_shown_by", []) or []:
            try:
                digital = get_jsonld(do_ref["id"])
            except requests.RequestException:
                continue
            for ap in digital.get("access_point", []) or []:
                m = MICRIO_RE.match(ap.get("id", ""))
                if m:
                    base = m.group(1)
                    return IIIFRef(base, base.rsplit("/", 1)[-1], title, obj_num)
    return None


def download_720(iiif_base: str, dest: Path) -> tuple[str, int]:
    url = f"{iiif_base}/full/!720,720/0/default.jpg"
    r = session.get(url, headers={"Accept": "image/jpeg"}, timeout=60)
    r.raise_for_status()
    dest.write_bytes(r.content)
    return url, len(r.content)


# ---------------------------------------------------------------------------
# Image dimension probe (no Pillow dependency — parse JPEG SOF marker).

def jpeg_dims(path: Path) -> tuple[int, int] | None:
    data = path.read_bytes()
    i = 2  # skip SOI
    while i < len(data) - 1:
        if data[i] != 0xFF:
            return None
        marker = data[i + 1]
        i += 2
        if marker == 0xD8 or marker == 0xD9:
            continue
        if 0xD0 <= marker <= 0xD7:
            continue
        if i + 2 > len(data):
            return None
        seg_len = int.from_bytes(data[i:i + 2], "big")
        if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7,
                      0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
            h = int.from_bytes(data[i + 3:i + 5], "big")
            w = int.from_bytes(data[i + 5:i + 7], "big")
            return w, h
        i += seg_len
    return None


# ---------------------------------------------------------------------------
# Main flow

@dataclass
class DownloadRow:
    bucket: str
    item_id: str
    title: str
    object_number: str | None
    file: str
    bytes: int
    dims: tuple[int, int] | None
    iiif_url: str


def download_from_bucket(bucket_label: str, items: list[dict], n: int,
                         errors: list[str]) -> list[DownloadRow]:
    rows: list[DownloadRow] = []
    for it in items:
        if len(rows) >= n:
            break
        try:
            ref = find_iiif(it["id"])
        except requests.RequestException as e:
            errors.append(f"[{bucket_label}] resolve {it['id']}: {e}")
            continue
        if ref is None:
            continue
        safe_label = re.sub(r"[^A-Za-z0-9._-]+", "_", bucket_label)
        fname = f"{safe_label}__{ref.micrio_id}.jpg"
        dest = IMG_DIR / fname
        try:
            url, size = download_720(ref.iiif_base, dest)
        except requests.RequestException as e:
            errors.append(f"[{bucket_label}] download {ref.iiif_base}: {e}")
            continue
        dims = jpeg_dims(dest)
        rows.append(DownloadRow(
            bucket=bucket_label,
            item_id=it["id"],
            title=ref.title,
            object_number=ref.object_number,
            file=fname,
            bytes=size,
            dims=dims,
            iiif_url=url,
        ))
        print(f"  downloaded {fname} ({size} bytes, {dims})")
    return rows


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    IMG_DIR.mkdir(parents=True, exist_ok=True)

    started = time.strftime("%Y-%m-%d %H:%M:%SZ", time.gmtime())
    errors: list[str] = []

    # --- 1. Sets
    print("Listing sets via OAI-PMH...")
    all_sets = list_sets()
    print(f"  {len(all_sets)} sets enumerated")
    chosen_sets = pick_demo_sets(all_sets)
    print(f"  chose: {[(s.spec, s.name) for s in chosen_sets]}")

    # --- 2. Artworks in each chosen set
    print("Walking set member pages...")
    set_walks: list[tuple[SetInfo, PageWalk]] = []
    for s in chosen_sets:
        walk = list_set_artworks(s.spec, max_items=10)
        print(f"  set {s.spec}: total={walk.total}, fetched={len(walk.items)} on {walk.pages_fetched} page(s)")
        set_walks.append((s, walk))

    # --- 3. Keyword searches
    print("Running keyword searches (title + description fan-out)...")
    searches: list[KeywordSearch] = []
    for kw in KEYWORDS:
        ks = keyword_search(kw)
        print(f"  '{kw}': field_totals={ks.field_totals}, union={len(ks.union_items)}")
        searches.append(ks)

    # --- 4. Downloads
    print("Downloading images at IIIF /full/!720,720/0/default.jpg ...")
    download_rows: list[DownloadRow] = []
    for s, walk in set_walks:
        label = f"set-{s.spec}"
        download_rows.extend(download_from_bucket(label, walk.items, DOWNLOADS_PER_BUCKET, errors))
    for ks in searches:
        label = f"search-{ks.keyword}"
        download_rows.extend(download_from_bucket(label, ks.union_items, DOWNLOADS_PER_BUCKET, errors))

    # --- 5. Report
    print(f"Writing report to {REPORT_PATH} ...")
    write_report(started, all_sets, chosen_sets, set_walks, searches, download_rows, errors)
    print(f"Done. {len(download_rows)} images downloaded; {len(errors)} non-fatal errors.")
    return 0


def write_report(started: str,
                 all_sets: list[SetInfo],
                 chosen_sets: list[SetInfo],
                 set_walks: list[tuple[SetInfo, PageWalk]],
                 searches: list[KeywordSearch],
                 downloads: list[DownloadRow],
                 errors: list[str]) -> None:
    out = io.StringIO()
    p = lambda *a: print(*a, file=out)

    p("# Rijksmuseum exploration report")
    p()
    p(f"- Run started: `{started}`")
    p(f"- Source: Rijksmuseum (Amsterdam) — anonymous Linked Open Data + IIIF")
    p(f"- Endpoints: `{BASE}` (Linked Art + OAI-PMH), `https://iiif.micr.io/` (IIIF Image API v2)")
    p(f"- Image size requested: longest side = 720 px (`/full/!720,720/0/default.jpg`)")
    p()
    p("## Feature support summary")
    p()
    p("| UBI feature | Native? | Notes |")
    p("|---|---|---|")
    p("| List collections | partial | OAI-PMH `verb=ListSets` enumerates curated Sets in a single XML response. No JSON-LD listing endpoint. |")
    p("| List artworks in a collection | cursor only | `search/collection?memberOfSetId=…` returns 100 items per page with a `pageToken` cursor. **Range-style pagination must be emulated** by walking the cursor and indexing locally. |")
    p("| Keyword search | emulated | No free-text `q`. The API accepts scoped fields (`title`, `description`, `creator`, `aboutActor`, `material`, `technique`, `type`, `creationDate`, `objectNumber`). This script fans out across `title` + `description` and unions the results. |")
    p()

    # Section 1
    p("## 1. List collections (curated Sets)")
    p()
    p(f"`GET {OAI}?verb=ListSets` returned **{len(all_sets)} sets** (Dutch + English names mixed).")
    p()
    p("First 15 sets:")
    p()
    p("| setSpec | setName |")
    p("|---|---|")
    for s in all_sets[:15]:
        p(f"| `{s.spec}` | {s.name} |")
    p()
    p(f"Chosen for demo: " + ", ".join(f"`{s.spec}` ({s.name})" for s in chosen_sets))
    p()

    # Section 2
    p("## 2. List artworks in 2 collections (cursor pagination)")
    p()
    for s, walk in set_walks:
        p(f"### Set `{s.spec}` — {s.name}")
        p()
        p(f"- Endpoint: `GET {SEARCH}?memberOfSetId={ID_PREFIX}{s.spec}&imageAvailable=true`")
        p(f"- Total items in set with images: **{walk.total}**")
        p(f"- Cursor pages fetched: {walk.pages_fetched}; collected first {len(walk.items)} item IDs.")
        p(f"- Sample IDs: " + ", ".join(f"`{it['id'].rsplit('/', 1)[-1]}`" for it in walk.items[:5]))
        p()

    # Section 3
    p("## 3. Keyword search")
    p()
    p("**Emulation note:** The data.rijksmuseum.nl API has no free-text `q` parameter — only scoped fields are accepted. Each keyword below is dispatched against `title` and `description` separately, and the script unions the result sets client-side. A real UBI adapter would either index the corpus locally for true full-text search or always present search as scoped.")
    p()
    for ks in searches:
        p(f"### Search: `{ks.keyword}`")
        p()
        p(f"- Endpoint per field: `GET {SEARCH}?<field>={urllib.parse.quote(ks.keyword)}&imageAvailable=true`")
        for f, c in ks.field_totals.items():
            p(f"- field `{f}` → {c} total hits")
        p(f"- Union (deduped) sample IDs: " + ", ".join(f"`{it['id'].rsplit('/', 1)[-1]}`" for it in ks.union_items[:5]))
        p()

    # Section 4
    p("## 4. Image downloads (720px longest side)")
    p()
    p("**IIIF discovery chain** (3 hops in Linked Art):")
    p("  `HumanMadeObject.shows[]` → `VisualItem.digitally_shown_by[]` → `DigitalObject.access_point[]` → IIIF URL on `iiif.micr.io`.")
    p()
    p("**Image request**: `GET {iiif_base}/full/!720,720/0/default.jpg` (IIIF Image API v2 `!w,h` = best-fit within box, so longest side = 720).")
    p()
    if downloads:
        p("| Bucket | Object # | Title | File | Bytes | Width × Height |")
        p("|---|---|---|---|---|---|")
        for d in downloads:
            dims = f"{d.dims[0]} × {d.dims[1]}" if d.dims else "?"
            title = (d.title or "").replace("|", "\\|")[:60]
            obj = d.object_number or "—"
            p(f"| `{d.bucket}` | {obj} | {title} | `{d.file}` | {d.bytes} | {dims} |")
        p()
        p(f"Total downloaded: **{len(downloads)} files** in `out/images/`.")
        # Sanity check: every image should have one dimension == 720
        bad = [d for d in downloads if d.dims and 720 not in d.dims]
        if bad:
            p()
            p(f"⚠ {len(bad)} image(s) did NOT report 720 on either axis (the IIIF server may have downscaled below the requested fit):")
            for d in bad:
                p(f"  - `{d.file}`: {d.dims}")
    else:
        p("_No images were downloaded._")
    p()

    if errors:
        p("## Non-fatal errors")
        p()
        for e in errors:
            p(f"- {e}")
        p()

    REPORT_PATH.write_text(out.getvalue(), encoding="utf-8")


if __name__ == "__main__":
    sys.exit(main())
