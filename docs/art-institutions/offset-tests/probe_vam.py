"""
Probe V&A's offset capabilities on /v2/objects/search.

The firmware uses ?page=N&page_size=100&images_exist=1 with no offset
knob. V&A's API is built on a Solr-style backend; Solr supports
`start=N` and sometimes `cursorMark` style pagination. We check
whether V&A exposes either, plus the usual sweep for deep-page
failure modes.

Filter: id_category=THES48903 ("Prints"), 104,429 records — plenty
of depth to test offset behavior.

Tests:
  1. Baseline page=1&page_size=10 — confirm response shape.
  2. page=2&page_size=10 — non-overlap check.
  3. page=1&page_size=20 — superset confirmation.
  4. Try start=10&page_size=10 (Solr-style).
  5. Try cursor=*&page_size=10 (cursorMark-style).
  6. Try from=10&page_size=10.
  7. Try offset=10&page_size=10.
  8. Big page: page=100&page_size=100 (items 9901..10000).
  9. page=200, 500, 1000 — deeper.
 10. Maximum page_size — try page_size=1000, 5000.
 11. Sort param availability.
"""

import json
import time
import urllib.parse
import urllib.request

BASE = "https://api.vam.ac.uk/v2"
PRINTS = ("id_category", "THES48903")  # 104,429 records


def fetch_with_headers(url: str) -> tuple[int, dict | None, str, dict]:
    req = urllib.request.Request(url)
    req.add_header("Accept", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=25) as r:
            body = r.read()
            status = r.status
            headers = dict(r.getheaders())
    except urllib.error.HTTPError as e:
        body = e.read()
        status = e.code
        headers = dict(e.headers.items())
    raw_head = body[:400].decode("utf-8", errors="replace")
    try:
        data = json.loads(body.decode("utf-8"))
    except Exception:
        data = None
    return status, data, raw_head, headers


def section(label: str) -> None:
    print()
    print("=" * 78)
    print(label)
    print("=" * 78)


def build_url(filter_pair, **params) -> str:
    fk, fv = filter_pair
    q = [("images_exist", "1"), (fk, fv)]
    for k, v in params.items():
        q.append((k, str(v)))
    return f"{BASE}/objects/search?" + urllib.parse.urlencode(q)


def probe(label: str, url: str, ref_ids: list | None = None) -> tuple[int, list, dict]:
    status, data, head, headers = fetch_with_headers(url)
    ids: list = []
    info_dict = {}
    print(f"  [{status}] {label}")
    if status == 200 and data:
        info_dict = data.get("info", {})
        # V&A flat shape: info contains record_count, page, page_size, pages
        slim = {k: info_dict.get(k) for k in
                ("record_count", "record_count_exact", "page", "page_size", "pages", "image_count")}
        print(f"    info: {slim}")
        for r in data.get("records", []):
            sid = r.get("systemNumber")
            if sid:
                ids.append(sid)
        if ids:
            print(f"    {len(ids)} ids; first 5: {ids[:5]}")
        if ref_ids is not None:
            overlap = set(ids) & set(ref_ids)
            print(f"    overlap with reference: {len(overlap)}")
            if ids == ref_ids:
                print(f"    *** EQUIVALENT to reference page ***")
    else:
        print(f"    body head: {head[:240]}")
    return status, ids, info_dict


def main() -> None:
    section("Test 1 — Baseline page=1&page_size=10 on Prints (THES48903)")
    url = build_url(PRINTS, page=1, page_size=10)
    print(f"  URL: {url}")
    _, ids_p1, _ = probe("page=1 page_size=10", url)
    time.sleep(0.8)

    section("Test 2 — page=2&page_size=10 — confirm non-overlap")
    url = build_url(PRINTS, page=2, page_size=10)
    _, ids_p2, _ = probe("page=2 page_size=10", url, ref_ids=ids_p1)
    time.sleep(0.8)

    section("Test 3 — page=1&page_size=20 — confirm equals p1+p2")
    url = build_url(PRINTS, page=1, page_size=20)
    _, ids_p1_20, _ = probe("page=1 page_size=20", url)
    expected = ids_p1 + ids_p2
    print(f"  match p1+p2 order? {ids_p1_20 == expected}")
    time.sleep(0.8)

    section("Test 4 — start=10&page_size=10 (Solr-style)")
    url = build_url(PRINTS, start=10, page_size=10)
    probe("start=10 page_size=10", url, ref_ids=ids_p2)
    time.sleep(0.8)

    section("Test 5 — cursor=*&page_size=10 (cursorMark-style)")
    url = build_url(PRINTS, cursor="*", page_size=10)
    probe("cursor=* page_size=10", url, ref_ids=ids_p1)
    time.sleep(0.8)

    section("Test 6 — from=10&page_size=10")
    url = build_url(PRINTS, **{"from": 10, "page_size": 10})
    probe("from=10 page_size=10", url, ref_ids=ids_p2)
    time.sleep(0.8)

    section("Test 7 — offset=10&page_size=10")
    url = build_url(PRINTS, offset=10, page_size=10)
    probe("offset=10 page_size=10", url, ref_ids=ids_p2)
    time.sleep(0.8)

    section("Test 8 — Big page=100&page_size=100 (items 9901..10000)")
    url = build_url(PRINTS, page=100, page_size=100)
    probe("page=100 page_size=100", url)
    time.sleep(0.8)

    section("Test 9 — Deep-page sweep")
    for page in [10, 50, 100, 200, 500, 1000, 2000, 5000]:
        url = build_url(PRINTS, page=page, page_size=100)
        probe(f"page={page} page_size=100", url)
        time.sleep(1.0)

    section("Test 10 — Maximum page_size values")
    for ps in [100, 200, 500, 1000, 5000]:
        url = build_url(PRINTS, page=1, page_size=ps)
        probe(f"page=1 page_size={ps}", url)
        time.sleep(1.0)

    section("Test 11 — Sort param availability (does ordering stay deterministic?)")
    # Try a few common sort directives.
    for sort in ["systemNumber", "_score", "+systemNumber"]:
        url = build_url(PRINTS, page=1, page_size=5, sort=sort)
        probe(f"page=1 page_size=5 sort={sort}", url)
        time.sleep(0.8)

    section("Test 12 — Repeated identical requests — is ordering stable?")
    # Stability matters for offset: if order shifts between calls,
    # offset slicing is unsafe.
    url = build_url(PRINTS, page=1, page_size=5)
    _, ids_a, _ = probe("call 1", url)
    time.sleep(2.0)
    _, ids_b, _ = probe("call 2", url)
    print(f"  Stable order? {ids_a == ids_b}")


if __name__ == "__main__":
    main()
