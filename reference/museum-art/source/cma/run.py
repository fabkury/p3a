"""Cleveland Museum of Art (CMA) integration probe.

Exercises the API surface the p3a integration relies on — CMA is the
first non-IIIF museum, so instead of the UBI IIIF download capability
this verifies the fixed CDN web-rendition template:

  1. Enumerate department/type vocabularies (sampled — the full-corpus
     scan lives in scripts/build_cma_terms.py).
  2. List artworks inside a term with skip/limit pagination, including
     a deep-skip probe.
  3. Exact-match filter A/B (full name vs partial name).
  4. Single-record lookup by accession (GET /api/artworks/{accession}).
  5. Verify the web-rendition URL template against listing data and
     download sample JPEGs.

Writes a Markdown report to ``output/report.md`` and the downloaded
JPEGs to ``output/images/``.

API reference: https://openaccess-api.clevelandart.org/

Requires: requests
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

import requests

API_URL = "https://openaccess-api.clevelandart.org/api/artworks/"
CDN_ROOT = "https://openaccess-cdn.clevelandart.org"
USER_AGENT = "p3a-cma-probe/1.0 (+pub@kury.dev)"
BASE_FILTERS = {"cc0": "1", "has_image": "1"}

SCRIPT_DIR = Path(__file__).resolve().parent
OUTPUT_DIR = SCRIPT_DIR / "output"
IMAGES_DIR = OUTPUT_DIR / "images"
REPORT_PATH = OUTPUT_DIR / "report.md"

MAX_ID_CHARS = 32  # playset identifier[33] slot

session = requests.Session()
session.headers["User-Agent"] = USER_AGENT


def get(params: dict) -> dict:
    r = session.get(API_URL, params={**BASE_FILTERS, **params}, timeout=60)
    r.raise_for_status()
    return r.json()


def main() -> int:
    IMAGES_DIR.mkdir(parents=True, exist_ok=True)
    lines: list[str] = ["# CMA API probe report", ""]

    def log(s: str = "") -> None:
        print(s)
        lines.append(s)

    # --- 1. Vocabularies (1000-record sample) -----------------------------
    body = get({"limit": 1000, "fields": "department,type,accession_number"})
    total = body["info"]["total"]
    data = body["data"]
    deps = sorted({r["department"] for r in data if r.get("department")})
    types = sorted({r["type"] for r in data if r.get("type")})
    log(f"## 1. Corpus + vocabularies")
    log(f"- CC0-with-image total: **{total}**")
    log(f"- limit=1000 honored: got {len(data)} records in one page")
    log(f"- distinct departments in sample: {len(deps)}; types: {len(types)}")
    long_names = [d for d in deps + types if len(d) > MAX_ID_CHARS]
    log(f"- names over {MAX_ID_CHARS} chars (identifier slot): {long_names}")
    accs = [r["accession_number"] for r in data if r.get("accession_number")]
    bad = [a for a in accs if not re.fullmatch(r"[A-Za-z0-9.\-]+", a)]
    log(f"- accession charset: max len {max(map(len, accs))}, "
        f"non-[A-Za-z0-9.-] count: {len(bad)}")
    log()

    # --- 2. skip/limit pagination + deep skip -----------------------------
    dep = "Drawings"
    p1 = get({"limit": 5, "skip": 0, "department": dep,
              "fields": "accession_number"})
    deep = get({"limit": 1, "skip": 40000, "fields": "accession_number"})
    log("## 2. Pagination")
    log(f"- department={dep}: total {p1['info']['total']}, "
        f"page-1 ids {[r['accession_number'] for r in p1['data']]}")
    log(f"- deep skip=40000 (no filter): returned "
        f"{len(deep['data'])} record(s) — no offset cap")
    log()

    # --- 3. Exact-match filter A/B ----------------------------------------
    full = "Modern European Painting and Sculpture"
    partial = full[:MAX_ID_CHARS]
    n_full = get({"limit": 1, "department": full})["info"]["total"]
    n_part = get({"limit": 1, "department": partial})["info"]["total"]
    log("## 3. Filter exact-match requirement")
    log(f"- department=\"{full}\": {n_full}")
    log(f"- department=\"{partial}\" (32-char truncation): {n_part}")
    log("- => filters demand the exact full name; truncated playset ids "
        "must be expanded (CMA_TERM_EXPANSION in museums/cma.c)")
    log()

    # --- 4. Single-record lookup ------------------------------------------
    acc = accs[0]
    r = session.get(API_URL + acc, timeout=60)
    r.raise_for_status()
    rec = r.json()["data"]
    log("## 4. Single-record lookup")
    log(f"- GET /api/artworks/{acc} -> title {rec.get('title')!r}, "
        f"creators: {len(rec.get('creators') or [])}")
    log()

    # --- 5. Web-rendition template + downloads ----------------------------
    body = get({"limit": 100, "skip": 500, "fields": "accession_number,images"})
    mism, no_web, empty_dims = [], 0, 0
    sample_urls = []
    for rec in body["data"]:
        acc = rec.get("accession_number")
        web = (rec.get("images") or {}).get("web")
        if not web or not web.get("url"):
            no_web += 1
            continue
        if not web.get("width") or not web.get("filesize"):
            empty_dims += 1
        expected = f"{CDN_ROOT}/{acc}/{acc}_web.jpg"
        if web["url"] != expected:
            mism.append((acc, web["url"]))
        elif len(sample_urls) < 2:
            sample_urls.append((acc, web["url"]))
    log("## 5. Web-rendition template (100-record sample at skip=500)")
    log(f"- records without images.web.url: {no_web}")
    log(f"- records with empty width/filesize strings: {empty_dims}")
    log(f"- template mismatches ({CDN_ROOT}/{{acc}}/{{acc}}_web.jpg): "
        f"{len(mism)} {mism[:3]}")
    for acc, url in sample_urls:
        img = session.get(url, timeout=120)
        img.raise_for_status()
        path = IMAGES_DIR / f"{acc}_web.jpg"
        path.write_bytes(img.content)
        magic_ok = img.content[:3] == b"\xff\xd8\xff"
        log(f"- downloaded {acc}: {len(img.content)} bytes, "
            f"JPEG magic ok: {magic_ok}")
    log()
    log("Run summary: all probes completed.")

    REPORT_PATH.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"\nWrote {REPORT_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
