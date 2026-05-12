"""
Probe AIC escape hatches that might bypass the empirical 1000-cap on
/artworks/search.

Hatches tested:
  1. Range queries on the search endpoint (id range, date_start range)
     combined with the facet filter. If supported, we can partition
     any large facet into <1000-record sub-buckets.
  2. POST /artworks/search with full Elasticsearch DSL bodies:
     - bool filter + range
     - sort + search_after
     - aggregations
     - scroll API (unlikely but worth trying)
  3. IIIF Collection / Presentation manifest endpoints.
     Try various paths under /api/v1, /iiif/2, /iiif/3.
  4. OAI-PMH endpoint. Most museums expose it at /oai or /oai-pmh.

Filter used: artwork_type_id=1 (Painting, ~3794 records). This is the
facet that originally exposed the cap.
"""

import json
import time
import urllib.parse
import urllib.request

USER_AGENT = "p3a-offset-probe/1.0 (pub@kury.dev)"
ART_BASE = "https://api.artic.edu/api/v1"


def get(url: str, accept: str = "application/json") -> tuple[int, dict | str | None, str]:
    req = urllib.request.Request(url)
    req.add_header("AIC-User-Agent", USER_AGENT)
    req.add_header("Accept", accept)
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            body = r.read()
            status = r.status
            ctype = r.headers.get("Content-Type", "")
    except urllib.error.HTTPError as e:
        body = e.read()
        status = e.code
        ctype = e.headers.get("Content-Type", "")
    raw_head = body[:600].decode("utf-8", errors="replace")
    parsed: dict | str | None
    if "json" in ctype.lower():
        try:
            parsed = json.loads(body.decode("utf-8"))
        except Exception:
            parsed = raw_head
    else:
        parsed = raw_head
    return status, parsed, raw_head


def post(url: str, body: dict, accept: str = "application/json") -> tuple[int, dict | str | None, str]:
    req = urllib.request.Request(
        url,
        data=json.dumps(body).encode(),
        method="POST",
    )
    req.add_header("AIC-User-Agent", USER_AGENT)
    req.add_header("Accept", accept)
    req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            body_bytes = r.read()
            status = r.status
            ctype = r.headers.get("Content-Type", "")
    except urllib.error.HTTPError as e:
        body_bytes = e.read()
        status = e.code
        ctype = e.headers.get("Content-Type", "")
    raw_head = body_bytes[:600].decode("utf-8", errors="replace")
    parsed: dict | str | None
    if "json" in ctype.lower():
        try:
            parsed = json.loads(body_bytes.decode("utf-8"))
        except Exception:
            parsed = raw_head
    else:
        parsed = raw_head
    return status, parsed, raw_head


def section(label: str) -> None:
    print()
    print("=" * 78)
    print(label)
    print("=" * 78)


def show(label: str, status: int, parsed) -> None:
    info = ""
    if isinstance(parsed, dict):
        if "data" in parsed:
            n = len(parsed["data"])
            pg = parsed.get("pagination", {})
            info = f"got {n} records, total={pg.get('total')}, offset={pg.get('offset')}"
            if n > 0:
                ids = [it.get("id") for it in parsed["data"][:5] if isinstance(it, dict)]
                info += f", first_ids={ids}"
        elif "error" in parsed:
            info = f"error={parsed.get('error')!r} detail={parsed.get('detail')!r}"
        else:
            info = f"keys={list(parsed.keys())[:6]}"
    else:
        info = str(parsed)[:200]
    print(f"  [{status}] {label}: {info}")


def main() -> None:
    section("HATCH 1 — Range queries on /artworks/search (GET)")
    # Combine the facet filter with a range filter on id.
    # If supported, we can partition the facet by ID buckets.
    queries = [
        # Basic range filter alone
        {"label": "range id [1..50]",
         "qs": [("query[range][id][gte]", "1"),
                ("query[range][id][lt]", "50"),
                ("fields", "id"), ("size", "5")]},
        # Range + facet term
        {"label": "term Painting + range id [1..1000]",
         "qs": [("query[term][artwork_type_id]", "1"),
                ("query[range][id][gte]", "1"),
                ("query[range][id][lt]", "1000"),
                ("fields", "id"), ("size", "5")]},
        # Higher bucket — paintings with id in [40000, 50000]
        {"label": "term Painting + range id [40000..50000]",
         "qs": [("query[term][artwork_type_id]", "1"),
                ("query[range][id][gte]", "40000"),
                ("query[range][id][lt]", "50000"),
                ("fields", "id"), ("size", "5")]},
        # Deep offset in the second bucket — if range works the cap is per-bucket
        {"label": "Painting + range id [40000..50000] + from=200 size=100",
         "qs": [("query[term][artwork_type_id]", "1"),
                ("query[range][id][gte]", "40000"),
                ("query[range][id][lt]", "50000"),
                ("fields", "id"), ("from", "200"), ("size", "100")]},
        # date_start range
        {"label": "Painting + range date_start [1900..1950] + from=200 size=10",
         "qs": [("query[term][artwork_type_id]", "1"),
                ("query[range][date_start][gte]", "1900"),
                ("query[range][date_start][lt]", "1950"),
                ("fields", "id,date_display"),
                ("from", "200"), ("size", "10")]},
        # Range alone should hit cap eventually
        {"label": "range id [1..200000] + from=999 size=1 (cumulative ok?)",
         "qs": [("query[range][id][gte]", "1"),
                ("query[range][id][lt]", "200000"),
                ("fields", "id"), ("from", "999"), ("size", "1")]},
    ]
    for q in queries:
        url = ART_BASE + "/artworks/search?" + urllib.parse.urlencode(q["qs"])
        status, parsed, head = get(url)
        show(q["label"], status, parsed)
        time.sleep(1.3)

    section("HATCH 2 — POST /artworks/search with ES DSL bodies")
    # The docs say POST is supported. Try several DSL shapes.
    bodies = [
        {"label": "bool filter (range) — count check",
         "body": {
             "query": {
                 "bool": {
                     "must": [{"term": {"artwork_type_id": 1}}],
                     "filter": [{"range": {"id": {"gte": 1, "lt": 50000}}}],
                 }
             },
             "size": 5,
             "fields": ["id"],
         }},
        {"label": "sort + search_after (cursor)",
         "body": {
             "query": {"term": {"artwork_type_id": 1}},
             "sort": [{"id": "asc"}],
             "search_after": [44084],   # id at offset 900
             "size": 5,
             "fields": ["id"],
         }},
        {"label": "from + size deep (should fail same as GET)",
         "body": {
             "query": {"term": {"artwork_type_id": 1}},
             "from": 1500,
             "size": 100,
             "fields": ["id"],
         }},
        {"label": "scroll (Elasticsearch scroll API style)",
         "body": {
             "query": {"term": {"artwork_type_id": 1}},
             "size": 100,
             "scroll": "1m",
             "fields": ["id"],
         }},
        {"label": "aggregations only — total bucket counts",
         "body": {
             "size": 0,
             "aggs": {
                 "by_type": {
                     "terms": {"field": "artwork_type_id", "size": 50}
                 }
             },
         }},
        {"label": "bool filter to slice — narrow range with from=900",
         "body": {
             "query": {
                 "bool": {
                     "must": [{"term": {"artwork_type_id": 1}}],
                     "filter": [{"range": {"id": {"gte": 100000, "lt": 200000}}}],
                 }
             },
             "sort": [{"id": "asc"}],
             "from": 900,
             "size": 100,
             "fields": ["id"],
         }},
    ]
    for b in bodies:
        url = ART_BASE + "/artworks/search"
        status, parsed, head = post(url, b["body"])
        show(b["label"], status, parsed)
        time.sleep(1.3)

    section("HATCH 3 — IIIF Collection / Presentation manifests")
    iiif_urls = [
        # AIC's IIIF v2 image base — does it have a manifest tree?
        "https://www.artic.edu/iiif/2/collection",
        "https://www.artic.edu/iiif/2/collection.json",
        # IIIF v3
        "https://www.artic.edu/iiif/3/collection",
        # Artwork-level manifests — pick known id
        "https://www.artic.edu/iiif/2/111628/manifest.json",
        "https://api.artic.edu/api/v1/artworks/111628/manifest.json",
        # Try IIIF collection at api.artic.edu
        "https://api.artic.edu/api/v1/iiif/collection",
        # Try IIIF for a department
        "https://api.artic.edu/api/v1/departments/PC-4/iiif",
        # Try iiif.artic.edu
        "https://iiif.artic.edu/",
        "https://iiif.artic.edu/collection",
    ]
    for url in iiif_urls:
        status, parsed, head = get(url, accept="application/json")
        info = parsed if isinstance(parsed, dict) else (head[:200] if isinstance(head, str) else "")
        if isinstance(info, dict):
            keys = list(info.keys())[:8]
            info = f"keys={keys}"
        print(f"  [{status}] {url}: {str(info)[:240]}")
        time.sleep(0.8)

    section("HATCH 4 — OAI-PMH endpoint discovery")
    oai_urls = [
        "https://api.artic.edu/oai",
        "https://api.artic.edu/oai-pmh",
        "https://api.artic.edu/oai?verb=Identify",
        "https://api.artic.edu/api/v1/oai",
        "https://www.artic.edu/oai",
        "https://www.artic.edu/api/oai",
        "https://www.artic.edu/oai?verb=Identify",
    ]
    for url in oai_urls:
        status, parsed, head = get(url, accept="application/xml")
        snippet = head[:160].replace("\n", " ").strip() if isinstance(head, str) else ""
        print(f"  [{status}] {url}: {snippet}")
        time.sleep(0.8)


if __name__ == "__main__":
    main()
