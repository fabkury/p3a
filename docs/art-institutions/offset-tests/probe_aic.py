"""
Probe Art Institute of Chicago (AIC) offset capabilities.

We need to know whether AIC's /artworks/search endpoint supports
true offset queries (i.e. "give me items 3001-4000") in ways beyond
the current page=N approach. The firmware currently uses page+limit
only; the question is whether there's an additional knob we can use
to give multiple channels of the same axis/term independent slices.

Tests performed:
  1. Baseline page=1&limit=10 — confirm response shape, total cap.
  2. page=2&limit=10 — confirm non-overlap with page 1.
  3. page=1&limit=20 — confirm superset of pages 1+2 in same order.
  4. from=N parameter — Elasticsearch-style offset.
  5. offset=N parameter — alternate naming.
  6. size=N parameter — ES-style page-size alias.
  7. Big page: page=100&limit=100 (offset 9900) — pre-cap.
  8. Beyond cap: page=101&limit=100 (offset 10000) — at cap boundary.
  9. page=11 on Painting artwork_type — reproduce the 403 issue.
 10. ?ids=... batch fetch — confirms how AIC handles ID lists,
     orthogonal to offset but useful to know.
 11. Test ?sort=... + range — Elasticsearch search_after style?

We use a low-traffic facet (artworks classified as "Painting" is huge;
we'll use a small department like "Arts of Greece, Rome, and Byzantium"
PC-4 for most tests). For the deep-page 403 reproduction we use
artwork_types Painting where it was originally observed.
"""

import json
import sys
import time
import urllib.parse
import urllib.request

BASE = "https://api.artic.edu/api/v1"
USER_AGENT = "p3a-offset-probe/1.0 (pub@kury.dev)"

# A small department: "Arts of Greece, Rome, and Byzantium" — currently ~500 records
SMALL_FILTER = ("query[term][department_id]", "PC-4")

# A huge type: "Painting" — many thousands; used to reproduce the deep-page 403
HUGE_FILTER = ("query[term][artwork_type_id]", "1")


def fetch(url: str, extra_headers: dict | None = None) -> tuple[int, dict | None, str]:
    """Return (status, json, raw_body_head)."""
    req = urllib.request.Request(url)
    req.add_header("AIC-User-Agent", USER_AGENT)
    req.add_header("Accept", "application/json")
    if extra_headers:
        for k, v in extra_headers.items():
            req.add_header(k, v)
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            body = r.read()
            status = r.status
    except urllib.error.HTTPError as e:
        body = e.read()
        status = e.code
    raw_head = body[:200].decode("utf-8", errors="replace")
    try:
        data = json.loads(body.decode("utf-8"))
    except Exception:
        data = None
    return status, data, raw_head


def build_search_url(filter_pair, **params) -> str:
    """Build a search URL with the given filter and arbitrary extra params."""
    qkey, qval = filter_pair
    q = [(qkey, qval), ("fields", "id,title,image_id")]
    for k, v in params.items():
        q.append((k, str(v)))
    return f"{BASE}/artworks/search?" + urllib.parse.urlencode(q)


def ids_summary(data: dict | None) -> list[int]:
    if not data:
        return []
    return [item["id"] for item in data.get("data", [])]


def section(label: str) -> None:
    print()
    print("=" * 78)
    print(label)
    print("=" * 78)


def run() -> None:
    section("Test 1 — Baseline page=1&limit=10 on small filter (PC-4)")
    url = build_search_url(SMALL_FILTER, page=1, limit=10)
    print("URL:", url)
    status, data, head = fetch(url)
    print(f"Status: {status}")
    if data:
        pagination = data.get("pagination", {})
        print(f"pagination: {json.dumps(pagination, indent=2)}")
        ids_p1_l10 = ids_summary(data)
        print(f"first 10 ids: {ids_p1_l10}")
    else:
        print("Body head:", head)
        ids_p1_l10 = []

    time.sleep(1.2)
    section("Test 2 — page=2&limit=10 — confirm non-overlap and ordering")
    url = build_search_url(SMALL_FILTER, page=2, limit=10)
    print("URL:", url)
    status, data, head = fetch(url)
    print(f"Status: {status}")
    if data:
        ids_p2_l10 = ids_summary(data)
        print(f"next 10 ids: {ids_p2_l10}")
        overlap = set(ids_p1_l10) & set(ids_p2_l10)
        print(f"overlap with page 1: {len(overlap)} ids")
    else:
        print("Body head:", head)
        ids_p2_l10 = []

    time.sleep(1.2)
    section("Test 3 — page=1&limit=20 — should equal page1_l10 + page2_l10 in order")
    url = build_search_url(SMALL_FILTER, page=1, limit=20)
    print("URL:", url)
    status, data, head = fetch(url)
    print(f"Status: {status}")
    if data:
        ids_p1_l20 = ids_summary(data)
        expected = ids_p1_l10 + ids_p2_l10
        print(f"got 20 ids:   {ids_p1_l20}")
        print(f"expected:     {expected}")
        print(f"match: {ids_p1_l20 == expected}")
    else:
        print("Body head:", head)

    time.sleep(1.2)
    section("Test 4 — Try ES-style from=10&size=10 (no page param)")
    url = build_search_url(SMALL_FILTER, **{"from": 10, "size": 10})
    print("URL:", url)
    status, data, head = fetch(url)
    print(f"Status: {status}")
    if data:
        pagination = data.get("pagination", {})
        print(f"pagination: {json.dumps(pagination, indent=2)}")
        ids = ids_summary(data)
        print(f"ids: {ids}")
        if ids_p2_l10:
            print(f"matches page=2&limit=10? {ids == ids_p2_l10}")
    else:
        print("Body head:", head)

    time.sleep(1.2)
    section("Test 5 — Try offset=10&limit=10")
    url = build_search_url(SMALL_FILTER, offset=10, limit=10)
    print("URL:", url)
    status, data, head = fetch(url)
    print(f"Status: {status}")
    if data:
        pagination = data.get("pagination", {})
        print(f"pagination: {json.dumps(pagination, indent=2)}")
        ids = ids_summary(data)
        print(f"ids: {ids}")
        if ids_p2_l10:
            print(f"matches page=2&limit=10? {ids == ids_p2_l10}")
    else:
        print("Body head:", head)

    time.sleep(1.2)
    section("Test 6 — Try from=10&limit=10 (mix)")
    url = build_search_url(SMALL_FILTER, **{"from": 10, "limit": 10})
    print("URL:", url)
    status, data, head = fetch(url)
    print(f"Status: {status}")
    if data:
        pagination = data.get("pagination", {})
        print(f"pagination: {json.dumps(pagination, indent=2)}")
        ids = ids_summary(data)
        print(f"ids: {ids}")
        if ids_p2_l10:
            print(f"matches page=2&limit=10? {ids == ids_p2_l10}")
    else:
        print("Body head:", head)

    time.sleep(1.2)
    section("Test 7 — Big-offset legal: page=100&limit=100 (= items 9901..10000) on Painting")
    url = build_search_url(HUGE_FILTER, page=100, limit=100)
    print("URL:", url)
    status, data, head = fetch(url)
    print(f"Status: {status}")
    if data:
        pagination = data.get("pagination", {})
        print(f"pagination: {json.dumps(pagination, indent=2)}")
        ids = ids_summary(data)
        print(f"got {len(ids)} ids; first 5: {ids[:5]}")
    else:
        print("Body head:", head)

    time.sleep(1.2)
    section("Test 8 — At-cap: page=101&limit=100 (= items 10001..10100) on Painting")
    url = build_search_url(HUGE_FILTER, page=101, limit=100)
    print("URL:", url)
    status, data, head = fetch(url)
    print(f"Status: {status}")
    if data:
        pagination = data.get("pagination", {})
        print(f"pagination: {json.dumps(pagination, indent=2)}")
        ids = ids_summary(data)
        print(f"got {len(ids)} ids")
    else:
        print("Body head:", head)

    time.sleep(1.2)
    section("Test 9 — Reproduce 403 issue: page=11&limit=100 on Painting (artwork_type_id=1)")
    url = build_search_url(HUGE_FILTER, page=11, limit=100)
    print("URL:", url)
    status, data, head = fetch(url)
    print(f"Status: {status}")
    if data:
        pagination = data.get("pagination", {})
        print(f"pagination: {json.dumps(pagination, indent=2)}")
        ids = ids_summary(data)
        print(f"got {len(ids)} ids; first 5: {ids[:5]}")
    else:
        print("Body head:", head)

    time.sleep(1.2)
    section("Test 10 — Sort consistency: page=2&limit=10 vs page=1&limit=10&sort=id (sanity)")
    # If page+limit ordering is deterministic across requests, the test 3
    # match above already proves it. This double-checks with an explicit sort.
    url = build_search_url(SMALL_FILTER, page=1, limit=10, sort="id")
    print("URL:", url)
    status, data, head = fetch(url)
    print(f"Status: {status}")
    if data:
        ids_sorted = ids_summary(data)
        print(f"sorted-asc-by-id first 10: {ids_sorted}")
    else:
        print("Body head:", head)

    time.sleep(1.2)
    section("Test 11 — Pagination metadata in big-result response")
    # Asking AIC directly: what does it say total_pages / total is for Painting?
    url = build_search_url(HUGE_FILTER, page=1, limit=1)
    print("URL:", url)
    status, data, head = fetch(url)
    print(f"Status: {status}")
    if data:
        pagination = data.get("pagination", {})
        print(f"pagination: {json.dumps(pagination, indent=2)}")
    else:
        print("Body head:", head)


if __name__ == "__main__":
    try:
        run()
    except KeyboardInterrupt:
        sys.exit(1)
