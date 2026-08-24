"""Minneapolis Institute of Art (Mia) integration probe.

Exercises the API surface the p3a integration relies on — Mia is the
second non-IIIF museum, so instead of the UBI IIIF download capability
this verifies the raw-Elasticsearch search passthrough and the fixed S3
width-bucket renditions:

  1. PD-with-image corpus total + live aggregation enumeration for the
     four axes (classification/department/country/style), including a
     hazard scan of bucket keys (>32 UTF-8 bytes, phrase-query breakers,
     unpaired surrogates, slashes).
  2. Phrase-query listing with size/from pagination, plus the ES
     10k-window probe (bare [] past from+size = 10000).
  3. Single-record lookup via an id: query.
  4. Image bucket checks (400/800 exist, 720 does not, bogus id 403) and
     sample JPEG downloads with magic-byte verification.

Writes a Markdown report to ``output/report.md`` and the downloaded
JPEGs to ``output/images/``.

API reference: https://github.com/artsmia/collection-elasticsearch

Requires: requests
"""

from __future__ import annotations

import re
import sys
import urllib.parse
from pathlib import Path

import requests

API_ROOT = "https://search.artsmia.org/"
IMG_ROOT = "https://1.api.artsmia.org"
USER_AGENT = "p3a-mia-probe/1.0 (+pub@kury.dev)"
SCOPE = 'rights_type:"Public Domain" AND image:valid'
MAX_ID_BYTES = 32  # playset identifier[33] slot (bytes)

SCRIPT_DIR = Path(__file__).resolve().parent
OUTPUT_DIR = SCRIPT_DIR / "output"
IMAGES_DIR = OUTPUT_DIR / "images"
REPORT_PATH = OUTPUT_DIR / "report.md"

session = requests.Session()
session.headers["User-Agent"] = USER_AGENT


def search(query: str, **params) -> dict | list:
    url = API_ROOT + urllib.parse.quote(query, safe="")
    r = session.get(url, params=params, timeout=60)
    r.raise_for_status()
    return r.json()


def main() -> int:
    IMAGES_DIR.mkdir(parents=True, exist_ok=True)
    lines: list[str] = ["# Mia API probe report", ""]

    def log(s: str = "") -> None:
        print(s)
        lines.append(s)

    # --- 1. Corpus + aggregations -----------------------------------------
    body = search(SCOPE, size=0, aggs="all")
    total = body["hits"]["total"]
    aggs = body.get("aggregations", {})
    log("## 1. Corpus + live aggregations")
    log(f"- scope: `{SCOPE}`")
    log(f"- total: **{total['value']}** (relation {total['relation']})")
    for name in ["Classification", "Department", "Country", "Style"]:
        group = aggs.get(name, {})
        buckets = group.get("buckets", [])
        kept, over_bytes, breakers, malformed, slashes = 0, 0, 0, 0, 0
        for b in buckets:
            key = b.get("key", "")
            try:
                key.encode("utf-8")
            except UnicodeEncodeError:
                malformed += 1
                continue
            if '"' in key or "\\" in key:
                breakers += 1
                continue
            if len(key.encode("utf-8", "surrogatepass")) > MAX_ID_BYTES:
                over_bytes += 1
                continue
            if "/" in key:
                slashes += 1  # kept — identifiers never touch the filesystem
            kept += 1
        top = [(b["key"], b["doc_count"]) for b in buckets[:3]]
        log(f"- {name}: {len(buckets)} buckets, {kept} usable "
            f"(dropped: {over_bytes} over-{MAX_ID_BYTES}B, {breakers} quote/backslash, "
            f"{malformed} malformed-unicode; {slashes} kept with '/'), top: {top}")
    log()

    # --- 2. Listing + pagination + ES window ------------------------------
    q = f'classification:"Paintings" AND {SCOPE}'
    p1 = search(q, size=5, **{"from": 0})
    log("## 2. Listing + pagination")
    log(f"- `{q}`: total {p1['hits']['total']}, page-1 ids "
        f"{[h['_source']['id'] for h in p1['hits']['hits']]}")
    deep = search(SCOPE, size=1, **{"from": 9990})
    log(f"- from=9990: envelope OK, {len(deep['hits']['hits'])} hit(s)")
    # Past-window behavior is INCONSISTENT: observed both a bare [] (a
    # JSON array instead of the envelope) and HTTP 500, depending on the
    # query. Firmware handles both (500 -> fetch-error path, [] ->
    # empty-page guard) and the offset cap avoids the region entirely.
    url = API_ROOT + urllib.parse.quote(SCOPE, safe="")
    r = session.get(url, params={"size": 1, "from": 10000}, timeout=60)
    if r.status_code == 200:
        past = r.json()
        log(f"- from=10000 (past ES window): HTTP 200, bare list: "
            f"{isinstance(past, list)}")
    else:
        log(f"- from=10000 (past ES window): HTTP {r.status_code} "
            f"(alternate past-window failure shape)")
    log()

    # --- 3. Single-record lookup ------------------------------------------
    sample = p1["hits"]["hits"][0]["_source"]
    sid = sample["id"]
    one = search(f"id:{sid}", size=1)
    rec = one["hits"]["hits"][0]["_source"]
    log("## 3. Single-record lookup")
    log(f"- `id:{sid}` -> title {rec.get('title')!r}, artist {rec.get('artist')!r}, "
        f"dated {rec.get('dated')!r}")
    log()

    # --- 4. Image buckets --------------------------------------------------
    log("## 4. Image buckets")
    ids = [h["_source"]["id"] for h in p1["hits"]["hits"][:2]]
    for w in [400, 800, 720]:
        r = session.get(f"{IMG_ROOT}/{w}/{ids[0]}.jpg", timeout=30)
        log(f"- /{w}/{ids[0]}.jpg -> HTTP {r.status_code}, {len(r.content)} bytes")
    r = session.get(f"{IMG_ROOT}/800/999999999.jpg", timeout=30)
    log(f"- /800/999999999.jpg (bogus id) -> HTTP {r.status_code}")
    for i in ids:
        r = session.get(f"{IMG_ROOT}/800/{i}.jpg", timeout=60)
        r.raise_for_status()
        path = IMAGES_DIR / f"{i}_800.jpg"
        path.write_bytes(r.content)
        magic_ok = r.content[:3] == b"\xff\xd8\xff"
        log(f"- downloaded {i}: {len(r.content)} bytes, JPEG magic ok: {magic_ok}")
    log()
    log("Run summary: all probes completed.")

    REPORT_PATH.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"\nWrote {REPORT_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
